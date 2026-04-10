/*
 * conffile.c — global state for the conffile.h compatibility shim
 *
 * Defines the 'cfg' sentinel global and the static pools used by
 * cfg_gettsec / cfg_getsec (declared as extern in conffile.h).
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

#include "conffile.h"

/* The upstream files access 'cfg' as a passthrough token; all actual
 * configuration reads go through owntone_config.  This static struct acts
 * as a never-NULL sentinel so that cfg_getsec / cfg_gettsec can accept it
 * as their first argument without a NULL-check. */
static cfg_t cfg_sentinel = { .name = "root", .section_id = CFG_SEC_GENERAL };

cfg_t *cfg = &cfg_sentinel;

/* Ring-buffer pools for cfg_t and cfg_opt_t returned by cfg_gettsec /
 * cfg_getsec / cfg_getopt.  8 slots is more than sufficient because all
 * cfg calls happen on the single player/event thread. */
cfg_t     cfg_pool[CFG_POOL_SIZE];
cfg_opt_t cfgopt_pool[CFG_POOL_SIZE];
int       cfg_pool_idx    = 0;
int       cfgopt_pool_idx = 0;
