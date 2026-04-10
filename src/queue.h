/*
 * In-memory queue for the pipe-only OwnTone build.
 * Replaces the SQLite-backed db_queue_* functions.
 *
 * Copyright (C) 2025 OwnTone-Minimal contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __QUEUE_H__
#define __QUEUE_H__

#include <stdint.h>

enum data_kind {
  DATA_KIND_PIPE    = 3,
};

enum media_kind {
  MEDIA_KIND_MUSIC  = 1,
};

/* In-memory queue item (mirrors the fields of the old db_queue_item used here) */
struct db_queue_item {
  uint32_t id;          /* unique queue item id */
  uint32_t file_id;     /* synthetic id for the pipe */
  uint32_t pos;

  enum data_kind data_kind;
  enum media_kind media_kind;

  uint32_t song_length; /* milliseconds, 0 = endless */

  char *path;

  char *title;
  char *artist;
  char *album;
  char *genre;

  char *artwork_url;
};


/* ----- Initialisation ----- */

/*
 * Populate the single-item queue from the pipe path in owntone-settings.json.
 * Must be called during player_init(). No-op if path is NULL.
 */
void db_queue_set_pipe(const char *path);
const char *db_queue_get_pipe_path(void);


/* ----- Fetch (caller must call free_queue_item when done) ----- */

struct db_queue_item *db_queue_fetch_byitemid(uint32_t item_id);
struct db_queue_item *db_queue_fetch_bypos(int pos, char shuffle);

/* Always returns NULL – there is no next for a single pipe. */
struct db_queue_item *db_queue_fetch_next(uint32_t item_id, char shuffle);


/* ----- Mutation ----- */

/* Update the in-memory item metadata (title, artist, etc.) */
int db_queue_item_update(struct db_queue_item *qi);

/* No-ops retained for call-site compatibility */
int  db_queue_delete_byitemid(uint32_t item_id);
int  db_queue_clear(int id);


/* ----- Lifecycle ----- */

void free_queue_item(struct db_queue_item *qi, int content_only);

#endif /* !__QUEUE_H__ */
