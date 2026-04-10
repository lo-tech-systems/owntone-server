/*
 * conffile.h — compatibility shim for owntone-minimal
 *
 * The upstream airplay.c and raop.c include "conffile.h" and use the
 * libconfuse cfg_t API (cfg_gettsec, cfg_getsec, cfg_getbool, cfg_getstr,
 * cfg_getint, cfg_getopt, cfg_opt_getnbool).  This header re-implements
 * those types and functions on top of owntone_config.h so that the upstream
 * source files can be compiled without any modification.
 *
 * Copyright (C) 2025 OwnTone-Minimal contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __CONFFILE_H__
#define __CONFFILE_H__

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "owntone_config.h"


/* -------------------------------------------------------------------------
 * cfg_t – represents either a per-device "airplay" section or a named
 * global section ("general", "library", "airplay_shared").
 * ------------------------------------------------------------------------- */

#define CFG_SEC_GENERAL        0
#define CFG_SEC_LIBRARY        1
#define CFG_SEC_AIRPLAY_SHARED 2
#define CFG_SEC_DEVICE         3   /* per-device block */

struct _cfg_t {
  char name[256];   /* device name (CFG_SEC_DEVICE) or section name (others) */
  int  section_id;  /* one of CFG_SEC_* */
};

typedef struct _cfg_t cfg_t;

/* -------------------------------------------------------------------------
 * cfg_opt_t – holds the tri-state reconnect value for one device.
 * nvalues == 0  → key absent  (auto)
 * nvalues == 1  → key present, value in 'value' (0=false, 1=true)
 * ------------------------------------------------------------------------- */

struct _cfg_opt_t {
  int nvalues;
  int value;
};

typedef struct _cfg_opt_t cfg_opt_t;

/* -------------------------------------------------------------------------
 * Global cfg sentinel – upstream files reference 'cfg' without declaring it
 * themselves; they pick up the extern from this header.
 * The actual definition lives in conffile.c.
 * ------------------------------------------------------------------------- */

extern cfg_t *cfg;

/* -------------------------------------------------------------------------
 * Small static pools so we can return pointers without dynamic allocation.
 * All calls happen on a single event thread so a ring-buffer of 8 slots is
 * sufficient (airplay_init calls cfg_getsec three times in a row; device_cb
 * calls cfg_gettsec once per discovered device, never concurrently).
 * ------------------------------------------------------------------------- */

#define CFG_POOL_SIZE 8

/* Defined in conffile.c */
extern cfg_t     cfg_pool[CFG_POOL_SIZE];
extern cfg_opt_t cfgopt_pool[CFG_POOL_SIZE];
extern int       cfg_pool_idx;
extern int       cfgopt_pool_idx;

static inline cfg_t *cfg_pool_next(void)
{
  cfg_t *slot = &cfg_pool[cfg_pool_idx % CFG_POOL_SIZE];
  cfg_pool_idx++;
  return slot;
}

static inline cfg_opt_t *cfgopt_pool_next(void)
{
  cfg_opt_t *slot = &cfgopt_pool[cfgopt_pool_idx % CFG_POOL_SIZE];
  cfgopt_pool_idx++;
  return slot;
}

/* -------------------------------------------------------------------------
 * cfg_gettsec(cfg, section, name)
 *   section is always "airplay"; name is the mDNS device name.
 *   Always returns a non-NULL cfg_t so that optional per-device keys work
 *   via the config_get_device_* fallback mechanism.
 * ------------------------------------------------------------------------- */

static inline cfg_t *
cfg_gettsec(cfg_t *parent, const char *section, const char *name)
{
  cfg_t *slot;
  (void)parent;
  (void)section;
  slot = cfg_pool_next();
  slot->section_id = CFG_SEC_DEVICE;
  strncpy(slot->name, name ? name : "", sizeof(slot->name) - 1);
  slot->name[sizeof(slot->name) - 1] = '\0';
  return slot;
}

/* -------------------------------------------------------------------------
 * cfg_getsec(cfg, section)
 *   Returns a cfg_t for a named global section.
 * ------------------------------------------------------------------------- */

static inline cfg_t *
cfg_getsec(cfg_t *parent, const char *section)
{
  cfg_t *slot;
  (void)parent;
  slot = cfg_pool_next();
  strncpy(slot->name, section ? section : "", sizeof(slot->name) - 1);
  slot->name[sizeof(slot->name) - 1] = '\0';
  if (strcmp(section, "general") == 0)
    slot->section_id = CFG_SEC_GENERAL;
  else if (strcmp(section, "library") == 0)
    slot->section_id = CFG_SEC_LIBRARY;
  else if (strcmp(section, "airplay_shared") == 0)
    slot->section_id = CFG_SEC_AIRPLAY_SHARED;
  else
    slot->section_id = CFG_SEC_GENERAL; /* safe fallback */
  return slot;
}

/* -------------------------------------------------------------------------
 * cfg_getbool
 * ------------------------------------------------------------------------- */

static inline bool
cfg_getbool(cfg_t *sec, const char *key)
{
  if (!sec)
    return false;
  if (sec->section_id == CFG_SEC_DEVICE)
    return config_get_device_bool(sec->name, key, false);
  if (sec->section_id == CFG_SEC_AIRPLAY_SHARED)
    {
      if (strcmp(key, "uncompressed_alac") == 0)
        return config_get_bool("uncompressed_alac", false);
    }
  return false;
}

/* -------------------------------------------------------------------------
 * cfg_getstr
 * ------------------------------------------------------------------------- */

static inline char *
cfg_getstr(cfg_t *sec, const char *key)
{
  if (!sec)
    return NULL;
  if (sec->section_id == CFG_SEC_DEVICE)
    return (char *)config_get_device_str(sec->name, key, NULL);
  if (sec->section_id == CFG_SEC_GENERAL)
    {
      if (strcmp(key, "user_agent") == 0)
        return (char *)config_get_str("user_agent", PACKAGE_NAME "/" PACKAGE_VERSION);
    }
  if (sec->section_id == CFG_SEC_LIBRARY)
    {
      if (strcmp(key, "name") == 0)
        return (char *)config_get_str("server_name", PACKAGE_NAME);
    }
  return NULL;
}

/* -------------------------------------------------------------------------
 * cfg_getint
 * ------------------------------------------------------------------------- */

static inline int
cfg_getint(cfg_t *sec, const char *key)
{
  if (!sec)
    return 0;
  if (sec->section_id == CFG_SEC_DEVICE)
    return config_get_device_int(sec->name, key, 0);
  if (sec->section_id == CFG_SEC_AIRPLAY_SHARED)
    {
      if (strcmp(key, "timing_port") == 0)
        return config_get_int("airplay_timing_port", 0);
      if (strcmp(key, "control_port") == 0)
        return config_get_int("airplay_control_port", 0);
    }
  return 0;
}

/* -------------------------------------------------------------------------
 * cfg_getopt / cfg_opt_getnbool — reconnect tri-state
 * ------------------------------------------------------------------------- */

static inline cfg_opt_t *
cfg_getopt(cfg_t *sec, const char *key)
{
  cfg_opt_t *opt;
  int val;
  (void)key; /* always "reconnect" */
  if (!sec)
    return NULL;
  val = config_get_device_reconnect(sec->name);
  opt = cfgopt_pool_next();
  if (val == -1)
    {
      opt->nvalues = 0;
      opt->value   = 0;
    }
  else
    {
      opt->nvalues = 1;
      opt->value   = val; /* 0 or 1 */
    }
  return opt;
}

static inline bool
cfg_opt_getnbool(cfg_opt_t *opt, unsigned int idx)
{
  (void)idx;
  if (!opt || opt->nvalues == 0)
    return false;
  return (bool)opt->value;
}

#endif /* !__CONFFILE_H__ */
