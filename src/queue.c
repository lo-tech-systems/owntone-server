/*
 * In-memory queue implementation for the pipe-only OwnTone build.
 *
 * Copyright (C) 2026 James Pearce
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

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include "queue.h"
#include "logger.h"

/* The single queue item representing the configured pipe. Read and written
 * from several threads (player, worker, httpd), so every access to it -
 * including the deep copies handed out by the fetch functions - runs under
 * g_item_lck. Fetchers get their own copy back, never interior pointers,
 * so callers need no locking of their own. */
static struct db_queue_item g_item;
static int g_item_valid = 0;
static pthread_mutex_t g_item_lck = PTHREAD_MUTEX_INITIALIZER;

/* Populate the queue from the configured pipe path.
 * Called once from player_init(). */
void
db_queue_set_pipe(const char *path)
{
  if (!path)
    return;

  pthread_mutex_lock(&g_item_lck);

  if (g_item_valid)
    free_queue_item(&g_item, 1);

  memset(&g_item, 0, sizeof(g_item));
  g_item.id         = 1;
  g_item.file_id    = 1;
  g_item.pos        = 0;
  g_item.data_kind  = DATA_KIND_PIPE;
  g_item.media_kind = MEDIA_KIND_MUSIC;
  g_item.song_length = 0; /* endless */

  g_item.path         = strdup(path);
  g_item.title        = strdup("Pipe");
  g_item.artist       = strdup("");
  g_item.album        = strdup("");
  g_item.genre        = strdup("");

  g_item_valid = 1;

  pthread_mutex_unlock(&g_item_lck);

  DPRINTF(E_DBG, L_PLAYER, "In-memory queue configured for pipe '%s'\n", path);
}

// Returns an interior pointer, valid only until the next db_queue_set_pipe().
// Today both are only reached from the httpd thread (plus one-time player
// init), so this is safe; a caller on any other thread would need a copy
// taken under the lock instead.
const char *
db_queue_get_pipe_path(void)
{
  const char *path;

  pthread_mutex_lock(&g_item_lck);
  path = g_item_valid ? g_item.path : NULL;
  pthread_mutex_unlock(&g_item_lck);

  return path;
}

/* Deep-copy the global item and return the copy. */
static struct db_queue_item *
item_copy(void)
{
  struct db_queue_item *copy;

  if (!g_item_valid)
    return NULL;

  copy = calloc(1, sizeof(struct db_queue_item));
  if (!copy)
    return NULL;

  *copy = g_item;

#define DUP(f) copy->f = g_item.f ? strdup(g_item.f) : NULL
  DUP(path);
  DUP(title);
  DUP(artist);
  DUP(album);
  DUP(genre);
  DUP(artwork_url);
#undef DUP

  return copy;
}

struct db_queue_item *
db_queue_fetch_byitemid(uint32_t item_id)
{
  struct db_queue_item *copy;

  pthread_mutex_lock(&g_item_lck);
  copy = (g_item_valid && item_id == g_item.id) ? item_copy() : NULL;
  pthread_mutex_unlock(&g_item_lck);

  return copy;
}

struct db_queue_item *
db_queue_fetch_bypos(int pos, char shuffle)
{
  struct db_queue_item *copy;

  pthread_mutex_lock(&g_item_lck);
  copy = (g_item_valid && pos == 0) ? item_copy() : NULL;
  pthread_mutex_unlock(&g_item_lck);

  return copy;
}

/* A single-item queue has no next. */
struct db_queue_item *
db_queue_fetch_next(uint32_t item_id, char shuffle)
{
  return NULL;
}

int
db_queue_item_update(struct db_queue_item *qi)
{
  if (!qi)
    return -1;

  pthread_mutex_lock(&g_item_lck);

  if (!g_item_valid || qi->id != g_item.id)
    {
      pthread_mutex_unlock(&g_item_lck);
      return -1;
    }

#define UPD_STR(f) \
  if (qi->f) { free(g_item.f); g_item.f = strdup(qi->f); }
  UPD_STR(title);
  UPD_STR(artist);
  UPD_STR(album);
  UPD_STR(genre);
  UPD_STR(artwork_url);
#undef UPD_STR

  if (qi->song_length)
    g_item.song_length = qi->song_length;

  pthread_mutex_unlock(&g_item_lck);

  return 0;
}

int
db_queue_delete_byitemid(uint32_t item_id)
{
  return 0; /* no-op: pipe always stays in the queue */
}

int
db_queue_clear(int id)
{
  return 0; /* no-op */
}

void
free_queue_item(struct db_queue_item *qi, int content_only)
{
  if (!qi)
    return;

  free(qi->path);
  free(qi->title);
  free(qi->artist);
  free(qi->album);
  free(qi->genre);
  free(qi->artwork_url);

  if (!content_only)
    free(qi);
}
