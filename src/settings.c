/*
 * Copyright (C) 2019 Christian Meffert <christian.meffert@googlemail.com>
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

/*
 * Pipe-only build: all settings are stored in memory.
 * Values are not persisted across restarts (SQLite has been removed).
 */

#include "settings.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "misc.h"


static struct settings_option webinterface_options[] =
  {
      { "show_composer_now_playing", SETTINGS_TYPE_BOOL },
      { "show_filepath_now_playing", SETTINGS_TYPE_BOOL },
      { "show_composer_for_genre",   SETTINGS_TYPE_STR  },
      { "show_menu_item_playlists",  SETTINGS_TYPE_BOOL, { true }  },
      { "show_menu_item_music",      SETTINGS_TYPE_BOOL, { true }  },
      { "show_menu_item_podcasts",   SETTINGS_TYPE_BOOL, { true }  },
      { "show_menu_item_audiobooks", SETTINGS_TYPE_BOOL, { true }  },
      { "show_menu_item_radio",      SETTINGS_TYPE_BOOL, { false } },
      { "show_menu_item_files",      SETTINGS_TYPE_BOOL, { true }  },
      { "show_menu_item_search",     SETTINGS_TYPE_BOOL, { true }  },
      { "recently_added_limit",      SETTINGS_TYPE_INT,  { 100 }   },
  };

static struct settings_option artwork_options[] =
  {
      { "use_artwork_source_spotify",          SETTINGS_TYPE_BOOL, { true }  },
      { "use_artwork_source_discogs",          SETTINGS_TYPE_BOOL, { false } },
      { "use_artwork_source_coverartarchive",  SETTINGS_TYPE_BOOL, { false } },
      { "show_cover_artwork_in_album_lists",   SETTINGS_TYPE_BOOL, { true }  },
      { "streamurl_ignore",                    SETTINGS_TYPE_BOOL, { false } },
  };

static struct settings_option misc_options[] =
  {
      { "streamurl_keywords_artwork_url", SETTINGS_TYPE_STR },
      { "streamurl_keywords_length",      SETTINGS_TYPE_STR },
  };

static struct settings_option player_options[] =
  {
      { "player_mode_repeat",  SETTINGS_TYPE_INT  },
      { "player_mode_shuffle", SETTINGS_TYPE_BOOL },
      { "player_mode_consume", SETTINGS_TYPE_BOOL },
  };

static struct settings_category categories[] =
  {
      { "webinterface", webinterface_options, ARRAY_SIZE(webinterface_options) },
      { "artwork",      artwork_options,      ARRAY_SIZE(artwork_options)      },
      { "misc",         misc_options,         ARRAY_SIZE(misc_options)         },
      { "player",       player_options,       ARRAY_SIZE(player_options)       },
  };


/* ------------------------------ IMPLEMENTATION -----------------------------*/

int
settings_categories_count(void)
{
  return ARRAY_SIZE(categories);
}

struct settings_category *
settings_category_get_byindex(int index)
{
  if (index < 0 || settings_categories_count() <= index)
    return NULL;
  return &categories[index];
}

struct settings_category *
settings_category_get(const char *name)
{
  int i;

  for (i = 0; i < settings_categories_count(); i++)
    {
      if (strcasecmp(name, categories[i].name) == 0)
        return &categories[i];
    }

  return NULL;
}

int
settings_option_count(struct settings_category *category)
{
  return category->count_options;
}

struct settings_option *
settings_option_get(struct settings_category *category, const char *name)
{
  int i;

  if (!category || !name)
    return NULL;

  for (i = 0; i < category->count_options; i++)
    {
      if (strcasecmp(name, category->options[i].name) == 0)
        return &category->options[i];
    }

  return NULL;
}

struct settings_option *
settings_option_get_byindex(struct settings_category *category, int index)
{
  if (index < 0 || !category || category->count_options <= index)
    return NULL;

  return &category->options[index];
}


int
settings_option_getint(struct settings_option *option)
{
  if (!option || option->type != SETTINGS_TYPE_INT)
    return 0;

  if (option->is_set)
    return option->current_value.intval;

  return option->default_value.intval;
}

bool
settings_option_getbool(struct settings_option *option)
{
  if (!option || option->type != SETTINGS_TYPE_BOOL)
    return false;

  if (option->is_set)
    return option->current_value.boolval;

  return option->default_value.boolval;
}

char *
settings_option_getstr(struct settings_option *option)
{
  const char *s;

  if (!option || option->type != SETTINGS_TYPE_STR)
    return NULL;

  if (option->is_set)
    s = option->current_strval;
  else
    s = option->default_value.strval;

  return s ? strdup(s) : NULL;
}


int
settings_option_setint(struct settings_option *option, int value)
{
  if (!option || option->type != SETTINGS_TYPE_INT)
    return -1;

  option->current_value.intval = value;
  option->is_set = true;
  return 0;
}

int
settings_option_setbool(struct settings_option *option, bool value)
{
  if (!option || option->type != SETTINGS_TYPE_BOOL)
    return -1;

  option->current_value.boolval = value;
  option->is_set = true;
  return 0;
}

int
settings_option_setstr(struct settings_option *option, const char *value)
{
  char *copy;

  if (!option || option->type != SETTINGS_TYPE_STR)
    return -1;

  copy = value ? strdup(value) : NULL;
  free(option->current_strval);
  option->current_strval = copy;
  option->is_set = true;
  return 0;
}


int
settings_option_delete(struct settings_option *option)
{
  if (!option)
    return -1;

  option->is_set = false;
  free(option->current_strval);
  option->current_strval = NULL;
  memset(&option->current_value, 0, sizeof(option->current_value));
  return 0;
}
