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
 */

#ifndef __OWNTONE_CONFIG_H__
#define __OWNTONE_CONFIG_H__

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>  /* uid_t, gid_t */

// Fallback package identity if autoconf config.h is not available
#ifndef PACKAGE_NAME
# define PACKAGE_NAME "owntone-minimal"
#endif
#ifndef PACKAGE_VERSION
# define PACKAGE_VERSION "0.0.0"
#endif
#ifndef VERSION
# define VERSION PACKAGE_VERSION
#endif
#ifndef PACKAGE
# define PACKAGE PACKAGE_NAME
#endif

// Signal handling: signalfd is available on Linux, kqueue on BSD/macOS
#if defined(__linux__) && !defined(HAVE_SIGNALFD)
# define HAVE_SIGNALFD 1
#elif (defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)) && !defined(HAVE_KQUEUE)
# define HAVE_KQUEUE 1
#endif

// Default path for the settings file
#define SETTINGS_FILE  CONFDIR "/owntone-settings.json"

// Globals used by other modules (formerly in conffile.h)
extern uint64_t libhash;
extern uid_t    runas_uid;
extern gid_t    runas_gid;

// Load settings from JSON file. Must be called before any config_get_* calls.
// Returns 0 on success, -1 on error.
int  config_load(const char *path);

// Reload settings from the same file used by config_load().
// Only refreshes the key/value store; does not re-derive uid/libhash.
int  config_reload(void);

void config_unload(void);

// Read a string value. Returns fallback if the key is absent or null.
const char *config_get_str(const char *key, const char *fallback);

// Read an integer value. Returns fallback if absent or wrong type.
int  config_get_int(const char *key, int fallback);

// Read a boolean value. Returns fallback if absent or wrong type.
bool config_get_bool(const char *key, bool fallback);

// Read from a JSON array of strings (e.g. trusted_networks).
int         config_get_strlist_count(const char *key);
const char *config_get_strlist_item(const char *key, int index);

// Per-device AirPlay config. device_name is the mDNS-announced device name.
// The "airplay_devices" object in the JSON file maps device names to config objects.
const char *config_get_device_str(const char *device, const char *key, const char *fallback);
int         config_get_device_int(const char *device, const char *key, int fallback);
bool        config_get_device_bool(const char *device, const char *key, bool fallback);

// Persist a per-device string value (e.g. auth_key) into the JSON file.
// Creates the device entry and "airplay_devices" block if absent.
int         config_set_device_str(const char *device, const char *key, const char *value);

// Tri-state reconnect setting per device: -1=auto-detect, 0=disabled, 1=enabled.
// Absent or null in JSON means -1 (auto).
int         config_get_device_reconnect(const char *device);

// Update a setting at runtime and write back to the JSON file.
// config_is_api_settable() returns true only for keys the API is permitted to change.
bool config_is_api_settable(const char *key);
int  config_set_str(const char *key, const char *value);
int  config_set_int(const char *key, int value);
int  config_set_bool(const char *key, bool value);
bool config_restart_required_get(void);

#endif /* !__OWNTONE_CONFIG_H__ */
