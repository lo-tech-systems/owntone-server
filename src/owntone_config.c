/*
 * Copyright (C) 2026 James Pearce
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
#include <pthread.h>

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

// The settings store is read and written from several threads (the player
// thread persists per-device state, worker threads service the JSON API), and
// json-c object graphs are not safe for concurrent access. Every public
// accessor takes this lock; *_unlocked helpers exist for internal reuse.
// Strings returned by the getters are borrowed from the store, so callers
// must copy them before the value can be replaced.
static pthread_mutex_t config_lck = PTHREAD_MUTEX_INITIALIZER;


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
  // user_agent is deliberately absent here: absence means "derive from the
  // running build's PACKAGE_NAME/PACKAGE_VERSION" (see config_get_str()
  // callers), so the effective default always tracks the running build
  // instead of being frozen at whatever version first wrote the settings
  // file.
  json_object_object_add(defaults, "loglevel", json_object_new_int(3));
  json_object_object_add(defaults, "logfile", json_object_new_string("/var/log/owntone.log"));
  json_object_object_add(defaults, "pipe_path", json_object_new_string("/run/autostream-pipes/autostream.fifo"));
  json_object_object_add(defaults, "pipe_autostart", json_object_new_boolean(true));
  // Install default remains 44.1k/16-bit; the pipe format is API-settable
  // (see api_settable_keys[] below) and restart-coupled (see
  // config_key_requires_restart()) rather than reconciled at install time -
  // any move to a different default is a runtime/reconcile decision, not a
  // change to these built-in defaults.
  json_object_object_add(defaults, "pipe_sample_rate", json_object_new_int(44100));
  json_object_object_add(defaults, "pipe_bits_per_sample", json_object_new_int(16));
  // Read at filtergraph creation (per playback session), not cached at
  // startup, so a change takes effect on the next session with no restart -
  // see config_key_requires_restart() below, which deliberately omits this
  // key.
  json_object_object_add(defaults, "resample_quality", json_object_new_string("standard"));
  json_object_object_add(defaults, "ipv6", json_object_new_boolean(true));
  json_object_object_add(defaults, "start_buffer_ms", json_object_new_int(2250));
  // Default on: receivers advertising the lossless ALAC bufferStream get it
  // instead of AAC (better quality, far cheaper to encode - decisive on
  // small cores); receivers without it keep AAC, and the realtime path
  // sends uncompressed ALAC framing. LAN bandwidth cost is negligible.
  json_object_object_add(defaults, "uncompressed_alac", json_object_new_boolean(true));
  json_object_object_add(defaults, "buffered_audio_enabled", json_object_new_boolean(true));
  json_object_object_add(defaults, "buffered_encoder_budget", json_object_new_int(0));
  json_object_object_add(defaults, "device_removal_grace_period", json_object_new_int(180));
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
  "device_removal_grace_period",
  "buffered_audio_enabled",
  "buffered_encoder_budget",
  "pipe_sample_rate",
  "pipe_bits_per_sample",
  "resample_quality",
  "user_agent",
  NULL
};

static bool
config_key_requires_restart(const char *key)
{
  return (strcmp(key, "ipv6") == 0
       || strcmp(key, "start_buffer_ms") == 0
       || strcmp(key, "uncompressed_alac") == 0
       || strcmp(key, "pipe_sample_rate") == 0
       || strcmp(key, "pipe_bits_per_sample") == 0
       || strcmp(key, "user_agent") == 0);
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

static const char *
get_str_unlocked(const char *key, const char *fallback)
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

const char *
config_get_str(const char *key, const char *fallback)
{
  const char *ret;

  pthread_mutex_lock(&config_lck);
  ret = get_str_unlocked(key, fallback);
  pthread_mutex_unlock(&config_lck);
  return ret;
}

static int
get_int_unlocked(const char *key, int fallback)
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

int
config_get_int(const char *key, int fallback)
{
  int ret;

  pthread_mutex_lock(&config_lck);
  ret = get_int_unlocked(key, fallback);
  pthread_mutex_unlock(&config_lck);
  return ret;
}

static bool
get_bool_unlocked(const char *key, bool fallback)
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

bool
config_get_bool(const char *key, bool fallback)
{
  bool ret;

  pthread_mutex_lock(&config_lck);
  ret = get_bool_unlocked(key, fallback);
  pthread_mutex_unlock(&config_lck);
  return ret;
}

int
config_get_strlist_count(const char *key)
{
  json_object *arr;
  int ret = 0;

  pthread_mutex_lock(&config_lck);
  if (root && json_object_object_get_ex(root, key, &arr) && json_object_is_type(arr, json_type_array))
    ret = (int)json_object_array_length(arr);
  pthread_mutex_unlock(&config_lck);
  return ret;
}

const char *
config_get_strlist_item(const char *key, int index)
{
  json_object *arr;
  json_object *item;
  const char *ret = NULL;

  pthread_mutex_lock(&config_lck);
  if (root && json_object_object_get_ex(root, key, &arr) && json_object_is_type(arr, json_type_array)
      && index >= 0 && index < (int)json_object_array_length(arr))
    {
      item = json_object_array_get_idx(arr, index);
      if (item && json_object_is_type(item, json_type_string))
	ret = json_object_get_string(item);
    }
  pthread_mutex_unlock(&config_lck);
  return ret;
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
  json_object *dev;
  json_object *val;
  const char *ret = fallback;

  pthread_mutex_lock(&config_lck);
  dev = get_device_obj(device);
  if (dev && json_object_object_get_ex(dev, key, &val)
      && !json_object_is_type(val, json_type_null) && json_object_is_type(val, json_type_string))
    ret = json_object_get_string(val);
  pthread_mutex_unlock(&config_lck);
  return ret;
}

int
config_get_device_int(const char *device, const char *key, int fallback)
{
  json_object *dev;
  json_object *val;
  int ret = fallback;

  pthread_mutex_lock(&config_lck);
  dev = get_device_obj(device);
  if (dev && json_object_object_get_ex(dev, key, &val) && json_object_is_type(val, json_type_int))
    ret = json_object_get_int(val);
  pthread_mutex_unlock(&config_lck);
  return ret;
}

bool
config_get_device_bool(const char *device, const char *key, bool fallback)
{
  json_object *dev;
  json_object *val;
  bool ret = fallback;

  pthread_mutex_lock(&config_lck);
  dev = get_device_obj(device);
  if (dev && json_object_object_get_ex(dev, key, &val) && json_object_is_type(val, json_type_boolean))
    ret = (bool)json_object_get_boolean(val);
  pthread_mutex_unlock(&config_lck);
  return ret;
}

int
config_get_device_reconnect(const char *device)
{
  json_object *dev;
  json_object *val;
  int ret = -1;

  pthread_mutex_lock(&config_lck);
  dev = get_device_obj(device);
  if (dev && json_object_object_get_ex(dev, "reconnect", &val)
      && !json_object_is_type(val, json_type_null) && json_object_is_type(val, json_type_boolean))
    ret = json_object_get_boolean(val) ? 1 : 0;
  pthread_mutex_unlock(&config_lck);
  return ret;
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

static json_object *
set_device_obj_prepare(const char *device)
{
  json_object *devices;
  json_object *dev;

  if (!root || !device)
    return NULL;

  if (!json_object_object_get_ex(root, "airplay_devices", &devices))
    {
      devices = json_object_new_object();
      if (!devices)
        return NULL;
      json_object_object_add(root, "airplay_devices", devices);
    }

  if (!json_object_object_get_ex(devices, device, &dev))
    {
      dev = json_object_new_object();
      if (!dev)
        return NULL;
      json_object_object_add(devices, device, dev);
    }

  return dev;
}

int
config_set_device_str(const char *device, const char *key, const char *value)
{
  json_object *dev;
  int ret = -1;

  if (!key)
    return -1;

  pthread_mutex_lock(&config_lck);
  dev = set_device_obj_prepare(device);
  if (dev)
    {
      json_object_object_add(dev, key, value ? json_object_new_string(value) : NULL);
      ret = config_write();
    }
  pthread_mutex_unlock(&config_lck);

  return ret;
}

int
config_set_device_int(const char *device, const char *key, int value)
{
  json_object *dev;
  int ret = -1;

  if (!key)
    return -1;

  pthread_mutex_lock(&config_lck);
  dev = set_device_obj_prepare(device);
  if (dev)
    {
      json_object_object_add(dev, key, json_object_new_int(value));
      ret = config_write();
    }
  pthread_mutex_unlock(&config_lck);

  return ret;
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

  pthread_mutex_lock(&config_lck);
  current = get_str_unlocked(key, NULL);
  changed = ((current == NULL && value != NULL)
          || (current != NULL && value == NULL)
          || (current && value && strcmp(current, value) != 0));

  json_object_object_add(root, key, value ? json_object_new_string(value) : NULL);
  ret = config_write();
  if (ret == 0 && changed && config_key_requires_restart(key))
    restart_required_pending = true;
  pthread_mutex_unlock(&config_lck);

  return ret;
}

// Remove a key entirely, rather than storing JSON null (config_set_str()
// with a NULL value would do that instead), so a cleared setting leaves no
// trace in the settings file and readers fall back to their built-in default.
int
config_delete(const char *key)
{
  int changed;
  int ret;

  if (!config_is_api_settable(key))
    return -1;

  pthread_mutex_lock(&config_lck);
  changed = (get_str_unlocked(key, NULL) != NULL);
  json_object_object_del(root, key);
  ret = config_write();
  if (ret == 0 && changed && config_key_requires_restart(key))
    restart_required_pending = true;
  pthread_mutex_unlock(&config_lck);

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

  if (strcmp(key, "device_removal_grace_period") == 0 && (value < 0 || value > 3600))
    return -1;

  if (strcmp(key, "buffered_encoder_budget") == 0 && (value < 0 || value > 64))
    return -1;

  // Mirrors the accepted sets pipe.c enforces at init (pipe.c ~1467/1474);
  // rejecting bad values here stops the API from persisting a config that
  // would make pipe_setup() DPRINTF(E_FATAL) and abort startup.
  if (strcmp(key, "pipe_sample_rate") == 0
      && value != 44100 && value != 48000 && value != 88200 && value != 96000)
    return -1;

  if (strcmp(key, "pipe_bits_per_sample") == 0 && value != 16 && value != 32)
    return -1;

  // loglevel follows the requested severity, clamped to the supported range
  // instead of being rejected, so the persisted value always matches what
  // logger_severity_set() will actually apply (no config/runtime divergence).
  if (strcmp(key, "loglevel") == 0)
    value = (value < E_FATAL) ? E_FATAL : (value > E_SPAM ? E_SPAM : value);

  pthread_mutex_lock(&config_lck);
  current = get_int_unlocked(key, INT_MIN);
  json_object_object_add(root, key, json_object_new_int(value));
  ret = config_write();
  if (ret == 0 && current != value && config_key_requires_restart(key))
    restart_required_pending = true;
  pthread_mutex_unlock(&config_lck);

  return ret;
}

int
config_set_bool(const char *key, bool value)
{
  bool current;
  int ret;

  if (!config_is_api_settable(key))
    return -1;

  pthread_mutex_lock(&config_lck);
  current = get_bool_unlocked(key, !value);
  json_object_object_add(root, key, json_object_new_boolean(value));
  ret = config_write();
  if (ret == 0 && current != value && config_key_requires_restart(key))
    restart_required_pending = true;
  pthread_mutex_unlock(&config_lck);

  return ret;
}

bool
config_restart_required_get(void)
{
  bool ret;

  pthread_mutex_lock(&config_lck);
  ret = restart_required_pending;
  pthread_mutex_unlock(&config_lck);
  return ret;
}


/* ----------------------- Load / unload ------------------------------------ */

/*
 * Fill in any settings keys absent from the loaded file with their built-in
 * defaults, then persist the result. This migrates a settings file written by
 * an older version forward as new keys are introduced, so every reader
 * (playback, JSON API) sees one consistent source of truth in the file instead
 * of relying on per-call fallbacks. Existing values are never changed - only
 * missing top-level keys are added; nested objects such as airplay_devices are
 * added whole when absent but not merged into. Runs once at startup, before any
 * other thread exists (so the unlocked accessors are safe), and writes the file
 * only when a key was actually added.
 */
static int
config_heal_defaults(void)
{
  json_object *defaults;
  json_object *existing;
  int added = 0;

  if (!root)
    return -1;

  defaults = config_defaults_build();
  if (!defaults)
    {
      DPRINTF(E_LOG, L_CONF, "Could not build defaults for settings self-heal\n");
      return -1;
    }

  json_object_object_foreach(defaults, key, val)
    {
      if (json_object_object_get_ex(root, key, &existing))
        continue;

      // json_object_get() takes an extra ref so the value outlives the
      // defaults tree freed below; root then owns it.
      json_object_object_add(root, key, json_object_get(val));
      added++;
      DPRINTF(E_INFO, L_CONF, "Settings self-heal: added missing key '%s'\n", key);
    }

  json_object_put(defaults);

  if (added == 0)
    return 0;

  config_startup_actions |= CONFIG_STARTUP_KEYS_HEALED;
  DPRINTF(E_WARN, L_CONF, "Settings file was missing %d key(s); writing defaults to '%s'\n", added, config_path);

  return config_write();
}

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

  // Resolve the uid to run as. config_load() runs before any other thread
  // exists, so the unlocked getter is fine here.
  uid = get_str_unlocked("uid", "owntone");
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

  // Migrate a file from an older version forward by writing any missing keys.
  // The write happens as root here; config_ensure_access() (called next from
  // main) restores the intended owner and mode on the rewritten file.
  config_heal_defaults();

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

  pthread_mutex_lock(&config_lck);
  if (root)
    json_object_put(root);
  root = new_root;
  pthread_mutex_unlock(&config_lck);
  return 0;
}

void
config_unload(void)
{
  pthread_mutex_lock(&config_lck);
  if (root)
    {
      json_object_put(root);
      root = NULL;
    }
  free(config_path);
  config_path = NULL;
  restart_required_pending = false;
  config_startup_actions = CONFIG_STARTUP_UNCHANGED;
  pthread_mutex_unlock(&config_lck);
}
