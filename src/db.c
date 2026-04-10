/*
 * db.c — compatibility shim for owntone-minimal
 *
 * Implements db_speaker_save(), the only db_* function still called by
 * airplay.c and raop.c after the SQLite database was removed.
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
#include "owntone_config.h" /* config_set_device_str */
#include "db.h"

/* Persist the AirPlay pairing auth_key for this device into the
 * "airplay_devices" block of owntone-settings.json so it survives restart. */
void
db_speaker_save(struct output_device *device)
{
  if (!device)
    return;
  config_set_device_str(device->name, "auth_key", device->auth_key);
}
