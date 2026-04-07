/*
 * Copyright (C) 2025 OwnTone-Minimal contributors
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
 *
 *
 * About config.c
 * --------------
 * Reads owntone-settings.json and exposes a simple flat accessor API.
 * Replaces conffile.c / libconfuse.
 *
 * The JSON file is the single source of truth for both startup settings and
 * runtime-settable parameters.  API-settable keys are written back to the file
 * atomically (write to .tmp, then rename) on every PUT.
 *
 * Per-device AirPlay config lives under the "airplay_devices" object, keyed by
 * the mDNS-announced device name:
 *
 *   "airplay_devices": {
 *     "Living Room": { "max_volume": 11, "exclude": false, ... }
 *   }
 *
 * libhash is derived from the system hostname so that each host has a stable,
 * unique identity for AirPlay without requiring a user-visible server name.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>

#include <json.h>

#include "logger.h"
#include "misc.h"
#include "owntone_config.h"


/* --------------------------------- Globals -------------------------------- */

uint64_t libhash;
uid_t    runas_uid;
gid_t    runas_gid;


/* --------------------------------- State ---------------------------------- */

static json_object *root;
static char        *config_path;


/* -------------------- API-settable key registry --------------------------- */

static const char *api_settable_keys[] = {
  "loglevel",
  "pipe_autostart",
  "start_buffer_ms",
  "uncompressed_alac",
  NULL
};

bool
config_is_api_settable(const char *key)
{
  int i;
  for (i = 0; api_settable_keys[i]; i++)
    if (strcmp(key, api_settable_keys[i]) == 0)
      return true;
  return false;
}


/* ----------------------- Flat key accessors ------------------------------- */

const char *
config_get_str(const char *key, const char *fallback)
{
  json_object *val;

  if (!root)
    return fallback;
  if (!json_object_object_get_ex(root, key, &val))
    return fallback;
  if (json_object_is_type(val, json_type_null))
    return fallback;
  if (!json_object_is_type(val, json_type_string))
    return fallback;
  return json_object_get_string(val);
}

int
config_get_int(const char *key, int fallback)
{
  json_object *val;

  if (!root)
    return fallback;
  if (!json_object_object_get_ex(root, key, &val))
    return fallback;
  if (!json_object_is_type(val, json_type_int))
    return fallback;
  return json_object_get_int(val);
}

bool
config_get_bool(const char *key, bool fallback)
{
  json_object *val;

  if (!root)
    return fallback;
  if (!json_object_object_get_ex(root, key, &val))
    return fallback;
  if (!json_object_is_type(val, json_type_boolean))
    return fallback;
  return (bool)json_object_get_boolean(val);
}

int
config_get_strlist_count(const char *key)
{
  json_object *arr;

  if (!root)
    return 0;
  if (!json_object_object_get_ex(root, key, &arr))
    return 0;
  if (!json_object_is_type(arr, json_type_array))
    return 0;
  return (int)json_object_array_length(arr);
}

const char *
config_get_strlist_item(const char *key, int index)
{
  json_object *arr;
  json_object *item;

  if (!root)
    return NULL;
  if (!json_object_object_get_ex(root, key, &arr))
    return NULL;
  if (!json_object_is_type(arr, json_type_array))
    return NULL;
  if (index < 0 || index >= (int)json_object_array_length(arr))
    return NULL;
  item = json_object_array_get_idx(arr, index);
  if (!item || !json_object_is_type(item, json_type_string))
    return NULL;
  return json_object_get_string(item);
}


/* -------------------- Per-device AirPlay accessors ------------------------ */

static json_object *
get_device_obj(const char *device)
{
  json_object *devices;
  json_object *dev;

  if (!root || !device)
    return NULL;
  if (!json_object_object_get_ex(root, "airplay_devices", &devices))
    return NULL;
  if (!json_object_is_type(devices, json_type_object))
    return NULL;
  if (!json_object_object_get_ex(devices, device, &dev))
    return NULL;
  return dev;
}

const char *
config_get_device_str(const char *device, const char *key, const char *fallback)
{
  json_object *dev = get_device_obj(device);
  json_object *val;

  if (!dev)
    return fallback;
  if (!json_object_object_get_ex(dev, key, &val))
    return fallback;
  if (json_object_is_type(val, json_type_null))
    return fallback;
  if (!json_object_is_type(val, json_type_string))
    return fallback;
  return json_object_get_string(val);
}

int
config_get_device_int(const char *device, const char *key, int fallback)
{
  json_object *dev = get_device_obj(device);
  json_object *val;

  if (!dev)
    return fallback;
  if (!json_object_object_get_ex(dev, key, &val))
    return fallback;
  if (!json_object_is_type(val, json_type_int))
    return fallback;
  return json_object_get_int(val);
}

bool
config_get_device_bool(const char *device, const char *key, bool fallback)
{
  json_object *dev = get_device_obj(device);
  json_object *val;

  if (!dev)
    return fallback;
  if (!json_object_object_get_ex(dev, key, &val))
    return fallback;
  if (!json_object_is_type(val, json_type_boolean))
    return fallback;
  return (bool)json_object_get_boolean(val);
}

int
config_get_device_reconnect(const char *device)
{
  json_object *dev = get_device_obj(device);
  json_object *val;

  if (!dev)
    return -1;
  if (!json_object_object_get_ex(dev, "reconnect", &val))
    return -1;
  if (json_object_is_type(val, json_type_null))
    return -1;
  if (!json_object_is_type(val, json_type_boolean))
    return -1;
  return json_object_get_boolean(val) ? 1 : 0;
}


/* ----------------------- API write-back ----------------------------------- */

static int
config_write(void)
{
  char tmppath[PATH_MAX];
  FILE *fp;
  const char *json_str;
  int ret;

  if (!root || !config_path)
    return -1;

  snprintf(tmppath, sizeof(tmppath), "%s.tmp", config_path);

  json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
  if (!json_str)
    {
      DPRINTF(E_LOG, L_CONF, "Failed to serialise settings to JSON\n");
      return -1;
    }

  fp = fopen(tmppath, "w");
  if (!fp)
    {
      DPRINTF(E_LOG, L_CONF, "Could not open %s for writing: %s\n", tmppath, strerror(errno));
      return -1;
    }

  fputs(json_str, fp);
  fclose(fp);

  ret = rename(tmppath, config_path);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_CONF, "Could not rename %s to %s: %s\n", tmppath, config_path, strerror(errno));
      return -1;
    }

  return 0;
}

int
config_set_str(const char *key, const char *value)
{
  if (!config_is_api_settable(key))
    return -1;
  json_object_object_add(root, key, value ? json_object_new_string(value) : NULL);
  return config_write();
}

int
config_set_int(const char *key, int value)
{
  if (!config_is_api_settable(key))
    return -1;
  json_object_object_add(root, key, json_object_new_int(value));
  return config_write();
}

int
config_set_bool(const char *key, bool value)
{
  if (!config_is_api_settable(key))
    return -1;
  json_object_object_add(root, key, json_object_new_boolean(value));
  return config_write();
}


/* ----------------------- Load / unload ------------------------------------ */

int
config_load(const char *path)
{
  char hostname[HOST_NAME_MAX + 1];
  struct passwd *pw;
  const char *uid;

  config_path = strdup(path);
  if (!config_path)
    {
      fprintf(stderr, "config: out of memory\n");
      return -1;
    }

  root = json_object_from_file(path);
  if (!root)
    {
      fprintf(stderr, "config: could not parse '%s'\n", path);
      free(config_path);
      config_path = NULL;
      return -1;
    }

  // libhash: stable 64-bit identity derived from hostname, used as the
  // AirPlay device ID (Client-Instance / DACP-ID headers, PTP clock seed).
  if (gethostname(hostname, sizeof(hostname)) < 0)
    strncpy(hostname, "localhost", sizeof(hostname) - 1);
  libhash = murmur_hash64(hostname, strlen(hostname), 0);

  // Resolve the uid to run as
  uid = config_get_str("uid", "nobody");
  pw = getpwnam(uid);
  if (!pw)
    {
      fprintf(stderr, "config: could not look up user '%s': %s\n", uid, strerror(errno));
      json_object_put(root);
      root = NULL;
      free(config_path);
      config_path = NULL;
      return -1;
    }

  runas_uid = pw->pw_uid;
  runas_gid = pw->pw_gid;

  return 0;
}

void
config_unload(void)
{
  if (root)
    {
      json_object_put(root);
      root = NULL;
    }
  free(config_path);
  config_path = NULL;
}
