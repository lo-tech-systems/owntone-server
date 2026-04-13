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
#include <sys/stat.h>
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
static bool         restart_required_pending;
static enum config_startup_actions config_startup_actions;


/* ----------------------- Built-in default config ------------------------- */

static json_object *
config_defaults_build(void)
{
  json_object *defaults;
  json_object *trusted_networks;

  defaults = json_object_new_object();
  if (!defaults)
    return NULL;

  trusted_networks = json_object_new_array();
  if (!trusted_networks)
    {
      json_object_put(defaults);
      return NULL;
    }

  json_object_array_add(trusted_networks, json_object_new_string("lan"));

  json_object_object_add(defaults, "uid", json_object_new_string("owntone"));
  json_object_object_add(defaults, "server_name", json_object_new_string("owntone-mini"));
  json_object_object_add(defaults, "user_agent", json_object_new_string(PACKAGE_NAME "/" PACKAGE_VERSION));
  json_object_object_add(defaults, "loglevel", json_object_new_int(3));
  json_object_object_add(defaults, "logfile", json_object_new_string("/var/log/owntone.log"));
  json_object_object_add(defaults, "pipe_path", json_object_new_string("/tmp/owntone.fifo"));
  json_object_object_add(defaults, "pipe_autostart", json_object_new_boolean(true));
  json_object_object_add(defaults, "pipe_sample_rate", json_object_new_int(44100));
  json_object_object_add(defaults, "pipe_bits_per_sample", json_object_new_int(16));
  json_object_object_add(defaults, "ipv6", json_object_new_boolean(true));
  json_object_object_add(defaults, "start_buffer_ms", json_object_new_int(2250));
  json_object_object_add(defaults, "uncompressed_alac", json_object_new_boolean(false));
  json_object_object_add(defaults, "airplay_timing_port", json_object_new_int(0));
  json_object_object_add(defaults, "airplay_control_port", json_object_new_int(0));
  json_object_object_add(defaults, "trusted_networks", trusted_networks);
  json_object_object_add(defaults, "airplay_devices", json_object_new_object());

  return defaults;
}


/* -------------------- API-settable key registry --------------------------- */

static const char *api_settable_keys[] = {
  "loglevel",
  "pipe_path",
  "pipe_autostart",
  "ipv6",
  "start_buffer_ms",
  "uncompressed_alac",
  NULL
};

static bool
config_key_requires_restart(const char *key)
{
  return (strcmp(key, "ipv6") == 0
       || strcmp(key, "start_buffer_ms") == 0
       || strcmp(key, "uncompressed_alac") == 0);
}

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

static int config_write(void); /* forward declaration */
static int config_write_json_to_path(const char *path, json_object *obj);

static void
config_log_file_state(const char *path, const struct stat *sb)
{
  DPRINTF(E_DBG, L_CONF,
          "Settings file state for '%s': uid=%lu gid=%lu mode=%04o\n",
          path,
          (unsigned long)sb->st_uid,
          (unsigned long)sb->st_gid,
          (unsigned int)(sb->st_mode & 07777));
}

int
config_ensure_exists(const char *path)
{
  struct stat sb;
  json_object *defaults;
  int ret;

  config_startup_actions = CONFIG_STARTUP_UNCHANGED;
  DPRINTF(E_DBG, L_CONF, "Checking for settings file '%s'\n", path);

  ret = stat(path, &sb);
  if (ret == 0)
    {
      DPRINTF(E_DBG, L_CONF, "Settings file already exists: '%s'\n", path);
      return 0;
    }

  if (errno != ENOENT)
    {
      DPRINTF(E_LOG, L_CONF, "Could not stat settings file '%s': %s\n", path, strerror(errno));
      return -1;
    }

  DPRINTF(E_WARN, L_CONF, "Settings file missing, creating defaults at '%s'\n", path);

  defaults = config_defaults_build();
  if (!defaults)
    {
      DPRINTF(E_LOG, L_CONF, "Could not build default settings for '%s'\n", path);
      return -1;
    }

  ret = config_write_json_to_path(path, defaults);
  json_object_put(defaults);
  if (ret < 0)
    return -1;

  config_startup_actions |= CONFIG_STARTUP_BOOTSTRAPPED;
  DPRINTF(E_INFO, L_CONF, "Created default settings file '%s'\n", path);

  return 0;
}

int
config_ensure_access(void)
{
  struct stat sb;
  mode_t desired_mode = 0644;
  bool owner_mismatch;
  bool mode_mismatch;
  bool need_repair;
  bool attempted_repair = false;
  int ret;

  if (!config_path)
    return -1;

  ret = stat(config_path, &sb);
  if (ret < 0)
    {
      DPRINTF(E_WARN, L_CONF, "Could not stat settings file '%s' for access check: %s\n", config_path, strerror(errno));
      return 0;
    }

  if (!S_ISREG(sb.st_mode))
    {
      DPRINTF(E_WARN, L_CONF, "Settings path is not a regular file, skipping access self-heal: '%s'\n", config_path);
      return 0;
    }

  config_log_file_state(config_path, &sb);

  owner_mismatch = (sb.st_uid != runas_uid || sb.st_gid != runas_gid);
  mode_mismatch = ((sb.st_mode & 0777) != desired_mode);
  need_repair = owner_mismatch || mode_mismatch;

  if (!need_repair)
    {
      DPRINTF(E_INFO, L_CONF, "Settings file access already matches runtime user for '%s'\n", config_path);
      return 0;
    }

  if (owner_mismatch)
    {
      DPRINTF(E_WARN, L_CONF,
              "Settings file owner/group does not match runtime user for '%s' (found %lu:%lu, expected %lu:%lu)\n",
              config_path,
              (unsigned long)sb.st_uid,
              (unsigned long)sb.st_gid,
              (unsigned long)runas_uid,
              (unsigned long)runas_gid);
    }

  if (mode_mismatch)
    {
      DPRINTF(E_WARN, L_CONF,
              "Settings file mode for '%s' is %04o, expected %04o\n",
              config_path,
              (unsigned int)(sb.st_mode & 0777),
              (unsigned int)desired_mode);
    }

  if (geteuid() != (uid_t)0)
    {
      DPRINTF(E_WARN, L_CONF, "Settings file access needs repair but process is not running as root; continuing\n");
      return 0;
    }

  if (owner_mismatch)
    {
      ret = chown(config_path, runas_uid, runas_gid);
      attempted_repair = true;
      config_startup_actions |= CONFIG_STARTUP_ACCESS_SELF_HEAL;
      if (ret < 0)
        DPRINTF(E_WARN, L_CONF, "Could not change owner/group of settings file '%s': %s\n", config_path, strerror(errno));
    }

  if (mode_mismatch)
    {
      ret = chmod(config_path, desired_mode);
      attempted_repair = true;
      config_startup_actions |= CONFIG_STARTUP_ACCESS_SELF_HEAL;
      if (ret < 0)
        DPRINTF(E_WARN, L_CONF, "Could not change mode of settings file '%s' to %04o: %s\n",
                config_path, (unsigned int)desired_mode, strerror(errno));
    }

  if (attempted_repair)
    {
      ret = stat(config_path, &sb);
      if (ret == 0)
        {
          config_log_file_state(config_path, &sb);

          owner_mismatch = (sb.st_uid != runas_uid || sb.st_gid != runas_gid);
          mode_mismatch = ((sb.st_mode & 0777) != desired_mode);
          if (!owner_mismatch && !mode_mismatch)
            DPRINTF(E_INFO, L_CONF, "Settings file access self-heal completed for '%s'\n", config_path);
          else
            DPRINTF(E_WARN, L_CONF, "Settings file access still differs from the expected runtime user for '%s'; continuing\n", config_path);
        }
      else
        DPRINTF(E_WARN, L_CONF, "Could not re-stat settings file '%s' after access self-heal: %s\n", config_path, strerror(errno));
    }

  return 0;
}

enum config_startup_actions
config_startup_actions_get(void)
{
  return config_startup_actions;
}

int
config_set_device_str(const char *device, const char *key, const char *value)
{
  json_object *devices;
  json_object *dev;

  if (!root || !device || !key)
    return -1;

  if (!json_object_object_get_ex(root, "airplay_devices", &devices))
    {
      devices = json_object_new_object();
      if (!devices)
        return -1;
      json_object_object_add(root, "airplay_devices", devices);
    }

  if (!json_object_object_get_ex(devices, device, &dev))
    {
      dev = json_object_new_object();
      if (!dev)
        return -1;
      json_object_object_add(devices, device, dev);
    }

  json_object_object_add(dev, key, value ? json_object_new_string(value) : NULL);

  return config_write();
}


/* ----------------------- API write-back ----------------------------------- */

static int
config_write_json_to_path(const char *path, json_object *obj)
{
  char tmppath[PATH_MAX];
  FILE *fp;
  const char *json_str;
  bool direct_write = false;
  int ret;

  if (!path || !obj)
    return -1;

  snprintf(tmppath, sizeof(tmppath), "%s.tmp", path);

  json_str = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);
  if (!json_str)
    {
      DPRINTF(E_LOG, L_CONF, "Failed to serialise settings to JSON\n");
      return -1;
    }

  DPRINTF(E_DBG, L_CONF, "Writing settings file '%s'\n", path);

  fp = fopen(tmppath, "w");
  if (!fp)
    {
      /* If the config lives in a root-owned directory like /etc, the runtime
       * user may still have write access to the file itself but not be allowed
       * to create a sibling temporary file for atomic replace. Fall back to an
       * in-place overwrite in that case.
       */
      if (errno != EACCES && errno != EPERM)
        {
          DPRINTF(E_LOG, L_CONF, "Could not open %s for writing: %s\n", tmppath, strerror(errno));
          return -1;
        }

      fp = fopen(path, "w");
      if (!fp)
        {
          DPRINTF(E_LOG, L_CONF, "Could not open %s for writing: %s\n", path, strerror(errno));
          return -1;
        }

      direct_write = true;
      DPRINTF(E_DBG, L_CONF, "Falling back to direct settings write for '%s'\n", path);
    }

  ret = fputs(json_str, fp);
  if (ret >= 0)
    ret = fputc('\n', fp);
  if (ret < 0 || fclose(fp) != 0)
    {
      DPRINTF(E_LOG, L_CONF, "Could not write %s: %s\n", direct_write ? path : tmppath, strerror(errno));
      return -1;
    }

  if (direct_write)
    return 0;

  ret = rename(tmppath, path);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_CONF, "Could not rename %s to %s: %s\n", tmppath, path, strerror(errno));
      return -1;
    }

  return 0;
}

static int
config_write(void)
{
  if (!root || !config_path)
    return -1;

  return config_write_json_to_path(config_path, root);
}

int
config_set_str(const char *key, const char *value)
{
  const char *current;
  int changed;
  int ret;

  if (!config_is_api_settable(key))
    return -1;

  current = config_get_str(key, NULL);
  changed = ((current == NULL && value != NULL)
          || (current != NULL && value == NULL)
          || (current && value && strcmp(current, value) != 0));

  json_object_object_add(root, key, value ? json_object_new_string(value) : NULL);
  ret = config_write();
  if (ret == 0 && changed && config_key_requires_restart(key))
    restart_required_pending = true;

  return ret;
}

int
config_set_int(const char *key, int value)
{
  int current;
  int ret;

  if (!config_is_api_settable(key))
    return -1;

  if (strcmp(key, "start_buffer_ms") == 0 && (value < 300 || value > 3500))
    return -1;

  current = config_get_int(key, INT_MIN);
  json_object_object_add(root, key, json_object_new_int(value));
  ret = config_write();
  if (ret == 0 && current != value && config_key_requires_restart(key))
    restart_required_pending = true;

  return ret;
}

int
config_set_bool(const char *key, bool value)
{
  bool current;
  int ret;

  if (!config_is_api_settable(key))
    return -1;

  current = config_get_bool(key, !value);
  json_object_object_add(root, key, json_object_new_boolean(value));
  ret = config_write();
  if (ret == 0 && current != value && config_key_requires_restart(key))
    restart_required_pending = true;

  return ret;
}

bool
config_restart_required_get(void)
{
  return restart_required_pending;
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

  restart_required_pending = false;
  config_startup_actions &= ~CONFIG_STARTUP_ACCESS_SELF_HEAL;

  // libhash: stable 64-bit identity derived from hostname, used as the
  // AirPlay device ID (Client-Instance / DACP-ID headers, PTP clock seed).
  if (gethostname(hostname, sizeof(hostname)) < 0)
    strncpy(hostname, "localhost", sizeof(hostname) - 1);
  libhash = murmur_hash64(hostname, strlen(hostname), 0);

  // Resolve the uid to run as
  uid = config_get_str("uid", "owntone");
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

/*
 * Reload JSON from the same file used by config_load().
 * Does not re-derive libhash or runas_uid — only refreshes the key/value store.
 * Returns 0 on success, -1 if no path is set or the file cannot be parsed.
 */
int
config_reload(void)
{
  json_object *new_root;

  if (!config_path)
    return -1;

  new_root = json_object_from_file(config_path);
  if (!new_root)
    {
      DPRINTF(E_LOG, L_CONF, "config_reload: could not parse '%s'\n", config_path);
      return -1;
    }

  if (root)
    json_object_put(root);
  root = new_root;
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
  restart_required_pending = false;
  config_startup_actions = CONFIG_STARTUP_UNCHANGED;
}
