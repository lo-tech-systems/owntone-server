/*
 * db.c — compatibility shim for owntone-minimal
 *
 * Implements db_speaker_save(), the only db_* function still called by
 * airplay.c and raop.c after the SQLite database was removed. Also used by
 * airplay.c to persist a device's buffered-transport capability.
 *
 * Copyright (C) 2025 OwnTone-Minimal contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "outputs.h"        /* struct output_device (full definition) */
#include "owntone_config.h" /* config_set_device_str, config_set_device_int */
#include "db.h"

/* Persist the AirPlay pairing auth_key and the learned buffered-transport
 * capability bitmask for this device into the "airplay_devices" block of
 * owntone-settings.json so both survive a restart. */
void
db_speaker_save(struct output_device *device)
{
  if (!device)
    return;
  config_set_device_str(device->name, "auth_key", device->auth_key);
  config_set_device_int(device->name, "buffered_modes", (int)device->buffered_modes);
}
