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
#include <limits.h>  /* USHRT_MAX */

/* Keep in sync with data_kind_label[] */
enum data_kind {
  DATA_KIND_FILE    = 0,
  DATA_KIND_HTTP    = 1,
  DATA_KIND_SPOTIFY = 2,
  DATA_KIND_PIPE    = 3,
};

/* Keep in sync with media_kind_labels[] */
enum media_kind {
  MEDIA_KIND_MUSIC      = 1,
  MEDIA_KIND_MOVIE      = 2,
  MEDIA_KIND_PODCAST    = 4,
  MEDIA_KIND_AUDIOBOOK  = 8,
  MEDIA_KIND_MUSICVIDEO = 32,
  MEDIA_KIND_TVSHOW     = 64,
};

#define MEDIA_KIND_ALL USHRT_MAX

/* Magic id for items not stored in the files table */
#define DB_MEDIA_FILE_NON_PERSISTENT_ID 9999999

/* In-memory queue item (mirrors the old db_queue_item struct from db.h) */
struct db_queue_item {
  uint32_t id;          /* unique queue item id */
  uint32_t file_id;     /* id of the file in the files database (synthetic for pipe) */
  uint32_t pos;
  uint32_t shuffle_pos;

  enum data_kind data_kind;
  enum media_kind media_kind;

  uint32_t song_length; /* milliseconds, 0 = endless */

  char *path;
  char *virtual_path;

  char *title;
  char *artist;
  char *album_artist;
  char *album;
  char *genre;

  int64_t songalbumid;
  uint32_t time_modified;

  char *artist_sort;
  char *album_sort;
  char *album_artist_sort;

  uint32_t year;
  uint32_t track;
  uint32_t disc;

  char *artwork_url;

  uint32_t queue_version;

  char *composer;

  char *type;
  uint32_t bitrate;
  uint32_t samplerate;
  uint32_t channels;

  int64_t songartistid;

  /* Not saved in queue table */
  uint32_t seek;
};

#define qi_offsetof(field) offsetof(struct db_queue_item, field)


/* ----- Initialisation ----- */

/*
 * Populate the single-item queue from the pipe path in owntone-settings.json.
 * Must be called during player_init(). No-op if path is NULL.
 */
void db_queue_set_pipe(const char *path);


/* ----- Fetch (caller must call free_queue_item when done) ----- */

struct db_queue_item *db_queue_fetch_byitemid(uint32_t item_id);
struct db_queue_item *db_queue_fetch_bypos(int pos, char shuffle);

/* Always returns NULL – there is no next/previous for a single pipe. */
struct db_queue_item *db_queue_fetch_next(uint32_t item_id, char shuffle);
struct db_queue_item *db_queue_fetch_prev(uint32_t item_id, char shuffle);


/* ----- Mutation ----- */

/* Update the in-memory item metadata (title, artist, etc.) */
int db_queue_item_update(struct db_queue_item *qi);

/* No-ops for the pipe-only build */
int  db_queue_delete_byitemid(uint32_t item_id);
int  db_queue_clear(int id);
int  db_queue_reshuffle(uint32_t item_id);
void db_queue_inc_version(void);


/* ----- Lifecycle ----- */

void free_queue_item(struct db_queue_item *qi, int content_only);

#endif /* !__QUEUE_H__ */
