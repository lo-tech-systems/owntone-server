/*
 * Copyright (C) 2017 Christian Meffert <christian.meffert@googlemail.com>
 *
 * Adapted from httpd_adm.c:
 * Copyright (C) 2015 Stuart NAIFEH <stu@naifeh.org>
 *
 * Adapted from httpd_daap.c and httpd.c:
 * Copyright (C) 2009-2011 Julien BLACHE <jb@jblache.org>
 * Copyright (C) 2010 Kai Elwert <elwertk@googlemail.com>
 *
 * Adapted from mt-daapd:
 * Copyright (C) 2003-2007 Ron Pedde <ron@pedde.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "httpd_internal.h"
#include "conffile.h"
#include "db.h"
#ifdef LASTFM
# include "lastfm.h"
#endif
#include "logger.h"
#include "misc.h"
#include "misc_json.h"
#include "player.h"
#include "settings.h"
#include "smartpl_query.h"


static bool allow_modifying_stored_playlists;
static char *default_playlist_directory;
static inline void
safe_json_add_string(json_object *obj, const char *key, const char *value)
{
  if (value)
    json_object_object_add(obj, key, json_object_new_string(value));
}

static inline void
safe_json_add_string_from_int64(json_object *obj, const char *key, int64_t value)
{
  char tmp[100];
  int ret;

  if (value > 0)
    {
      ret = snprintf(tmp, sizeof(tmp), "%" PRIi64, value);
      if (ret < sizeof(tmp))
	json_object_object_add(obj, key, json_object_new_string(tmp));
    }
}

static inline void
safe_json_add_int_from_string(json_object *obj, const char *key, const char *value)
{
  int intval;
  int ret;

  if (!value)
    return;

  ret = safe_atoi32(value, &intval);
  if (ret == 0)
    json_object_object_add(obj, key, json_object_new_int(intval));
}

static inline void
safe_json_add_time_from_string(json_object *obj, const char *key, const char *value)
{
  uint32_t tmp;
  time_t timestamp;
  struct tm tm;
  char result[32];

  if (!value)
    return;

  if (safe_atou32(value, &tmp) != 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error converting timestamp to uint32_t: %s\n", value);
      return;
    }

  if (!tmp)
    return;

  timestamp = tmp;
  if (gmtime_r(&timestamp, &tm) == NULL)
    {
      DPRINTF(E_LOG, L_WEB, "Error converting timestamp to gmtime: %s\n", value);
      return;
    }

  strftime(result, sizeof(result), "%FT%TZ", &tm);

  json_object_object_add(obj, key, json_object_new_string(result));
}

static inline void
safe_json_add_date_from_string(json_object *obj, const char *key, const char *value)
{
  int64_t tmp;
  time_t timestamp;
  struct tm tm;
  char result[32];

  if (!value)
    return;

  if (safe_atoi64(value, &tmp) != 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error converting timestamp to int64_t: %s\n", value);
      return;
    }

  if (!tmp)
    return;

  timestamp = tmp;
  if (localtime_r(&timestamp, &tm) == NULL)
    {
      DPRINTF(E_LOG, L_WEB, "Error converting timestamp to localtime: %s\n", value);
      return;
    }

  strftime(result, sizeof(result), "%F", &tm);

  json_object_object_add(obj, key, json_object_new_string(result));
}
/* --------------------------- REPLY HANDLERS ------------------------------- */
static json_object *
option_get_json(struct settings_option *option)
{
  const char *optionname;
  json_object *json_option;
  int intval;
  bool boolval;
  char *strval;


  optionname = option->name;

  CHECK_NULL(L_WEB, json_option = json_object_new_object());
  json_object_object_add(json_option, "name", json_object_new_string(option->name));
  json_object_object_add(json_option, "type", json_object_new_int(option->type));

  if (option->type == SETTINGS_TYPE_INT)
    {
      intval = settings_option_getint(option);
      json_object_object_add(json_option, "value", json_object_new_int(intval));
    }
  else if (option->type == SETTINGS_TYPE_BOOL)
    {
      boolval = settings_option_getbool(option);
      json_object_object_add(json_option, "value", json_object_new_boolean(boolval));
    }
  else if (option->type == SETTINGS_TYPE_STR)
    {
      strval = settings_option_getstr(option);
      if (strval)
	{
	  json_object_object_add(json_option, "value", json_object_new_string(strval));
	  free(strval);
	}
    }
  else
    {
      DPRINTF(E_LOG, L_WEB, "Option '%s' has unknown type %d\n", optionname, option->type);
      jparse_free(json_option);
      return NULL;
    }

  return json_option;
}
static int
jsonapi_reply_settings_option_get(struct httpd_request *hreq)
{
  const char *categoryname;
  const char *optionname;
  struct settings_category *category;
  struct settings_option *option;
  json_object *jreply;


  categoryname = hreq->path_parts[2];
  optionname = hreq->path_parts[3];

  category = settings_category_get(categoryname);
  if (!category)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid category name '%s' given\n", categoryname);
      return HTTP_NOTFOUND;
    }

  option = settings_option_get(category, optionname);
  if (!option)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid option name '%s' given\n", optionname);
      return HTTP_NOTFOUND;
    }

  jreply = option_get_json(option);

  if (!jreply)
    {
      DPRINTF(E_LOG, L_WEB, "Error getting value for option '%s'\n", optionname);
      return HTTP_INTERNAL;
    }

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

static int
jsonapi_reply_settings_option_put(struct httpd_request *hreq)
{
  const char *categoryname;
  const char *optionname;
  struct settings_category *category;
  struct settings_option *option;
  json_object* request;
  int intval;
  bool boolval;
  const char *strval;
  int ret;


  categoryname = hreq->path_parts[2];
  optionname = hreq->path_parts[3];

  category = settings_category_get(categoryname);
  if (!category)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid category name '%s' given\n", categoryname);
      return HTTP_NOTFOUND;
    }

  option = settings_option_get(category, optionname);

  if (!option)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid option name '%s' given\n", optionname);
      return HTTP_NOTFOUND;
    }

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Missing request body for setting option '%s' (type %d)\n", optionname, option->type);
      return HTTP_BADREQUEST;
    }

  if (option->type == SETTINGS_TYPE_INT && jparse_contains_key(request, "value", json_type_int))
    {
      intval = jparse_int_from_obj(request, "value");
      ret = settings_option_setint(option, intval);
    }
  else if (option->type == SETTINGS_TYPE_BOOL && jparse_contains_key(request, "value", json_type_boolean))
    {
      boolval = jparse_bool_from_obj(request, "value");
      ret = settings_option_setbool(option, boolval);
    }
  else if (option->type == SETTINGS_TYPE_STR && jparse_contains_key(request, "value", json_type_string))
    {
      strval = jparse_str_from_obj(request, "value");
      ret = settings_option_setstr(option, strval);
    }
  else
    {
      DPRINTF(E_LOG, L_WEB, "Invalid value given for option '%s' (type %d): '%s'\n", optionname, option->type, json_object_to_json_string(request));
      return HTTP_BADREQUEST;
    }

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error changing setting '%s' (type %d) to '%s'\n", optionname, option->type, json_object_to_json_string(request));
      return HTTP_INTERNAL;
    }

  DPRINTF(E_INFO, L_WEB, "Setting option '%s.%s' changed to '%s'\n", categoryname, optionname, json_object_to_json_string(request));
  return HTTP_NOCONTENT;
}
/*
 * Endpoint to retrieve informations about the library
 *
 * Example response:
 *
 * {
 *  "artists": 84,
 *  "albums": 151,
 *  "songs": 3085,
 *  "db_playtime": 687824,
 *  "updating": false
 *}
 */
static int
jsonapi_reply_library(struct httpd_request *hreq)
{
  json_object *jreply;
  char *s;
  int ret;

  CHECK_NULL(L_WEB, jreply = json_object_new_object());

  ret = db_admin_get(&s, DB_ADMIN_START_TIME);
  if (ret == 0)
    {
      safe_json_add_time_from_string(jreply, "started_at", s);
      free(s);
    }

  json_object_object_add(jreply, "updating", json_object_new_boolean(false));
  json_object_object_add(jreply, "songs", json_object_new_int(1));
  json_object_object_add(jreply, "Healthy", json_object_new_boolean(true));

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));
  jparse_free(jreply);

  return HTTP_OK;
}


struct outputs_param
{
  json_object *output;
  uint64_t output_id;
  int output_volume;
};

static json_object *
speaker_to_json(struct player_speaker_info *spk)
{
  json_object *output;
  json_object *supported_formats;
  char output_id[21];
  enum media_format format;

  output = json_object_new_object();

  supported_formats = json_object_new_array();
  for (format = MEDIA_FORMAT_FIRST; format <= MEDIA_FORMAT_LAST; format = MEDIA_FORMAT_NEXT(format))
    {
      if (format & spk->supported_formats)
	json_object_array_add(supported_formats, json_object_new_string(media_format_to_string(format)));
    }

  snprintf(output_id, sizeof(output_id), "%" PRIu64, spk->id);
  json_object_object_add(output, "id", json_object_new_string(output_id));
  json_object_object_add(output, "name", json_object_new_string(spk->name));
  json_object_object_add(output, "type", json_object_new_string(spk->output_type));
  json_object_object_add(output, "selected", json_object_new_boolean(spk->selected));
  json_object_object_add(output, "has_password", json_object_new_boolean(spk->has_password));
  json_object_object_add(output, "requires_auth", json_object_new_boolean(spk->requires_auth));
  json_object_object_add(output, "needs_auth_key", json_object_new_boolean(spk->needs_auth_key));
  json_object_object_add(output, "volume", json_object_new_int(spk->absvol));
  json_object_object_add(output, "offset_ms", json_object_new_int(spk->offset_ms));
  json_object_object_add(output, "format", json_object_new_string(media_format_to_string(spk->format)));
  json_object_object_add(output, "supported_formats", supported_formats);

  return output;
}

static void
speaker_enum_cb(struct player_speaker_info *spk, void *arg)
{
  json_object *outputs;
  json_object *output;

  outputs = arg;

  output = speaker_to_json(spk);
  json_object_array_add(outputs, output);
}

/*
 * GET /api/outputs/[output_id]
 */
static int
jsonapi_reply_outputs_get_byid(struct httpd_request *hreq)
{
  struct player_speaker_info speaker_info;
  uint64_t output_id;
  json_object *jreply;
  int ret;

  ret = safe_atou64(hreq->path_parts[2], &output_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid output id given to outputs endpoint '%s'\n", hreq->path);

      return HTTP_BADREQUEST;
    }

  ret = player_speaker_get_byid(&speaker_info, output_id);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No output found for '%s'\n", hreq->path);

      return HTTP_BADREQUEST;
    }

  jreply = speaker_to_json(&speaker_info);
  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

/*
 * PUT /api/outputs/[output_id]
 */
static int
jsonapi_reply_outputs_put_byid(struct httpd_request *hreq)
{
  uint64_t output_id;
  json_object *request = NULL;
  bool selected;
  int volume;
  int offset_ms;
  const char *pin;
  const char *format;
  int ret;

  ret = safe_atou64(hreq->path_parts[2], &output_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid output id given to outputs endpoint '%s'\n", hreq->path);
      goto error;
    }

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse incoming request\n");
      goto error;
    }

  if (jparse_contains_key(request, "selected", json_type_boolean))
    {
      selected = jparse_bool_from_obj(request, "selected");
      ret = selected ? player_speaker_enable(output_id) : player_speaker_disable(output_id);
      if (ret < 0)
	goto error;
    }

  if (jparse_contains_key(request, "volume", json_type_int))
    {
      volume = jparse_int_from_obj(request, "volume");
      ret = player_volume_setabs_speaker(output_id, volume);
      if (ret < 0)
	goto error;
    }

  if (jparse_contains_key(request, "pin", json_type_string))
    {
      pin = jparse_str_from_obj(request, "pin");
      ret = pin ? player_speaker_authorize(output_id, pin) : 0;
      if (ret < 0)
	goto error;

    }

  if (jparse_contains_key(request, "format", json_type_string))
    {
      format = jparse_str_from_obj(request, "format");
      ret = format ? player_speaker_format_set(output_id, media_format_from_string(format)) : 0;
      if (ret < 0)
	goto error;
    }

  if (jparse_contains_key(request, "offset_ms", json_type_int))
    {
      offset_ms = jparse_int_from_obj(request, "offset_ms");
      ret = player_speaker_offset_ms_set(output_id, offset_ms);
      if (ret < 0)
	goto error;
    }

  jparse_free(request);
  return HTTP_NOCONTENT;

 error:
  jparse_free(request);
  return HTTP_BADREQUEST;
}
/*
 * Endpoint "/api/outputs"
 */
static int
jsonapi_reply_outputs(struct httpd_request *hreq)
{
  json_object *outputs;
  json_object *jreply;

  outputs = json_object_new_array();

  player_speaker_enumerate(speaker_enum_cb, outputs);

  jreply = json_object_new_object();
  json_object_object_add(jreply, "outputs", outputs);

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}
static int
jsonapi_reply_outputs_set(struct httpd_request *hreq)
{
  json_object *request;
  json_object *outputs;
  json_object *output_id;
  int nspk, i, ret;
  uint64_t *ids;

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse incoming request\n");
      return HTTP_BADREQUEST;
    }

  DPRINTF(E_DBG, L_WEB, "Received select-outputs post request: %s\n", json_object_to_json_string(request));

  ret = jparse_array_from_obj(request, "outputs", &outputs);
  if (ret == 0)
    {
      nspk = json_object_array_length(outputs);

      CHECK_NULL(L_WEB, ids = calloc((nspk + 1), sizeof(uint64_t)));
      ids[0] = nspk;

      ret = 0;
      for (i = 0; i < nspk; i++)
	{
	  output_id = json_object_array_get_idx(outputs, i);
	  ret = safe_atou64(json_object_get_string(output_id), &ids[i + 1]);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_WEB, "Failed to convert output id: %s\n", json_object_to_json_string(request));
	      break;
	    }
	}

      if (ret == 0)
	player_speaker_set(ids);

      free(ids);
    }
  else
    DPRINTF(E_LOG, L_WEB, "Missing outputs in request body: %s\n", json_object_to_json_string(request));

  jparse_free(request);

  return HTTP_NOCONTENT;
}

static int
play_item_with_id(const char *param)
{
  uint32_t item_id;
  struct db_queue_item *queue_item;
  int ret;

  ret = safe_atou32(param, &item_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid item id given '%s'\n", param);

      return HTTP_BADREQUEST;
    }

  queue_item = db_queue_fetch_byitemid(item_id);
  if (!queue_item)
    {
      DPRINTF(E_LOG, L_WEB, "No queue item with item id '%d'\n", item_id);

      return HTTP_BADREQUEST;
    }

  player_playback_stop();
  ret = player_playback_start_byitem(queue_item);
  free_queue_item(queue_item, 0);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to start playback from item with id '%d'\n", item_id);

      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
play_item_at_position(const char *param)
{
  uint32_t position;
  struct player_status status;
  struct db_queue_item *queue_item;
  int ret;

  ret = safe_atou32(param, &position);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid position given '%s'\n", param);

      return HTTP_BADREQUEST;
    }

  player_get_status(&status);

  queue_item = db_queue_fetch_bypos(position, status.shuffle);
  if (!queue_item)
    {
      DPRINTF(E_LOG, L_WEB, "No queue item at position '%d'\n", position);

      return HTTP_BADREQUEST;
    }

  player_playback_stop();
  ret = player_playback_start_byitem(queue_item);
  free_queue_item(queue_item, 0);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to start playback from position '%d'\n", position);

      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_play(struct httpd_request *hreq)
{
  const char *param;
  int ret;

  if ((param = httpd_query_value_find(hreq->query, "item_id")))
    {
      return play_item_with_id(param);
    }
  else if ((param = httpd_query_value_find(hreq->query, "position")))
    {
      return play_item_at_position(param);
    }

  ret = player_playback_start();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error starting playback.\n");
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}
static int
jsonapi_reply_player_stop(struct httpd_request *hreq)
{
  int ret;

  ret = player_playback_stop();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error stopping playback.\n");
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}
static struct httpd_uri_map adm_handlers[] =
  {
    /* Outputs */
    { HTTPD_METHOD_GET,    "^/api/outputs$",                               jsonapi_reply_outputs },
    { HTTPD_METHOD_GET,    "^/api/outputs/[[:digit:]]+$",                  jsonapi_reply_outputs_get_byid },
    { HTTPD_METHOD_PUT,    "^/api/outputs/[[:digit:]]+$",                  jsonapi_reply_outputs_put_byid },
    { HTTPD_METHOD_PUT,    "^/api/outputs/set$",                           jsonapi_reply_outputs_set },

    /* Player */
    { HTTPD_METHOD_PUT,    "^/api/player/stop$",                           jsonapi_reply_player_stop },
    { HTTPD_METHOD_PUT,    "^/api/player/play$",                           jsonapi_reply_player_play },

    /* Library (health check) */
    { HTTPD_METHOD_GET,    "^/api/library$",                               jsonapi_reply_library },

    /* Settings */
    { HTTPD_METHOD_GET,    "^/api/settings/[A-Za-z0-9_]+/[A-Za-z0-9_]+$",  jsonapi_reply_settings_option_get },
    { HTTPD_METHOD_PUT,    "^/api/settings/[A-Za-z0-9_]+/[A-Za-z0-9_]+$",  jsonapi_reply_settings_option_put },

    { 0, NULL, NULL }
  };


/* ------------------------------- JSON API --------------------------------- */

static void
jsonapi_request(struct httpd_request *hreq)
{
  int status_code;

  if (!httpd_request_is_authorized(hreq))
    {
      return;
    }

  if (!hreq->handler)
    {
      DPRINTF(E_LOG, L_WEB, "Unrecognized JSON API request: '%s'\n", hreq->uri);
      httpd_send_error(hreq, HTTP_BADREQUEST, "Bad Request");
      return;
    }

  status_code = hreq->handler(hreq);

  if (status_code >= 400)
    DPRINTF(E_LOG, L_WEB, "JSON api request failed with error code %d (%s)\n", status_code, hreq->uri);

  switch (status_code)
    {
      case HTTP_OK:                  /* 200 OK */
	httpd_header_add(hreq->out_headers, "Content-Type", "application/json");
	httpd_send_reply(hreq, status_code, "OK", HTTPD_SEND_NO_GZIP);
	break;
      case HTTP_NOCONTENT:           /* 204 No Content */
	httpd_send_reply(hreq, status_code, "No Content", HTTPD_SEND_NO_GZIP);
	break;
      case HTTP_NOTMODIFIED:         /* 304 Not Modified */
	httpd_send_reply(hreq, HTTP_NOTMODIFIED, NULL, HTTPD_SEND_NO_GZIP);
	break;
      case HTTP_BADREQUEST:          /* 400 Bad Request */
	httpd_send_error(hreq, status_code, "Bad Request");
	break;
      case 403:
	httpd_send_error(hreq, status_code, "Forbidden");
	break;
      case HTTP_NOTFOUND:            /* 404 Not Found */
	httpd_send_error(hreq, status_code, "Not Found");
	break;
      case HTTP_SERVUNAVAIL:            /* 503 */
        httpd_send_error(hreq, status_code, "Service Unavailable");
        break;
      case HTTP_INTERNAL:            /* 500 Internal Server Error */
      default:
	httpd_send_error(hreq, HTTP_INTERNAL, "Internal Server Error");
	break;
    }
}

static int
jsonapi_init(void)
{
  char *temp_path;

  default_playlist_directory = NULL;
  allow_modifying_stored_playlists = cfg_getbool(cfg_getsec(cfg, "library"), "allow_modifying_stored_playlists");
  if (allow_modifying_stored_playlists)
    {
      temp_path = cfg_getstr(cfg_getsec(cfg, "library"), "default_playlist_directory");
      if (temp_path)
	{
	  // The path in the conf file may have a trailing slash character. Return the realpath like it is done for the library directories.
	  default_playlist_directory = realpath(temp_path, NULL);
	  if (default_playlist_directory)
	    {
	      if (access(default_playlist_directory, W_OK) < 0)
	        DPRINTF(E_WARN, L_WEB, "Non-writable playlist save directory '%s'\n", default_playlist_directory);
	    }
	}

      if (!default_playlist_directory)
	{
	  DPRINTF(E_LOG, L_WEB, "Invalid playlist save directory, disabling modifying stored playlists\n");
	  allow_modifying_stored_playlists = false;
	}
     }

  return 0;
}

static void
jsonapi_deinit(void)
{
  free(default_playlist_directory);
}

struct httpd_module httpd_jsonapi =
{
  .name = "JSON API",
  .type = MODULE_JSONAPI,
  .logdomain = L_WEB,
  .subpaths = { "/api/", NULL },
  .handlers = adm_handlers,
  .init = jsonapi_init,
  .deinit = jsonapi_deinit,
  .request = jsonapi_request,
};
