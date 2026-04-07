/*
 * Copyright (C) 2014 Espen Jürgensen <espenjurgensen@gmail.com>
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

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <pthread.h>

#include <event2/event.h>
#include <sqlite3.h>

#include "owntone_config.h"
#include "logger.h"
#include "transcode.h"
#include "db.h"
#include "worker.h"
#include "cache.h"
#include "listener.h"
#include "commands.h"

struct cache_arg
{
  sqlite3 *hdl; // which cache database

  char *query; // daap query
  char *ua;    // user agent
  int is_remote;
  int msec;

  uint32_t id; // file id
  const char *header_format;

  const char *path;  // artwork path
  char *pathcopy;  // copy of artwork path (for async operations)
  int type;    // individual or group artwork
  int64_t persistentid;
  int max_w;
  int max_h;
  int format;
  time_t mtime;
  int cached;
  int del;

  struct evbuffer *evbuf;
};

struct cachelist
{
  uint32_t id;
  uint32_t ts;
};

struct cache_db_def
{
  const char *name;
  const char *create_query;
  const char *drop_query;
};


struct cache_xcode_job
{
  const char *format;
  char *file_path;
  int file_id;

  struct event *ev;
  bool is_encoding;

  struct evbuffer *header;
};


/* --------------------------------- GLOBALS -------------------------------- */

// cache thread
static pthread_t tid_cache;

// Event base, pipes and events
static struct event_base *evbase_cache;
static struct commands_base *cmdbase;

// State
static bool cache_is_initialized;

#define DB_DEF_ADMIN \
  { \
    "admin", \
    "CREATE TABLE IF NOT EXISTS admin(" \
    " key VARCHAR(32) PRIMARY KEY NOT NULL,"  \
    " value VARCHAR(32) NOT NULL" \
    ");", \
    "DROP TABLE IF EXISTS admin;", \
  }


// Transcoding cache
#define CACHE_XCODE_VERSION 1
#define CACHE_XCODE_NTHREADS 4
#define CACHE_XCODE_FORMAT_MP4 "mp4"
static sqlite3 *cache_xcode_hdl;
static struct event *cache_xcode_updateev;
static struct event *cache_xcode_prepareev;
static struct cache_xcode_job cache_xcode_jobs[CACHE_XCODE_NTHREADS];
static bool cache_xcode_is_enabled;
static struct cache_db_def cache_xcode_db_def[] = {
  DB_DEF_ADMIN,
  {
    "files",
    "CREATE TABLE IF NOT EXISTS files ("
    "   id                 INTEGER PRIMARY KEY NOT NULL,"
    "   time_modified      INTEGER DEFAULT 0,"
    "   filepath           VARCHAR(4096) NOT NULL"
    ");",
    "DROP TABLE IF EXISTS files;",
  },
  {
    "data",
    "CREATE TABLE IF NOT EXISTS data ("
    "   id                 INTEGER PRIMARY KEY NOT NULL,"
    "   timestamp          INTEGER DEFAULT 0,"
    "   file_id            INTEGER DEFAULT 0,"
    "   format             VARCHAR(255) NOT NULL,"
    "   header             BLOB,"
    "   UNIQUE(file_id, format) ON CONFLICT REPLACE"
    ");",
    "DROP TABLE IF EXISTS data;",
  },
};


/* --------------------------------- HELPERS -------------------------------- */


/* ---------------------------------- MAIN ---------------------------------- */
/*                                Thread: cache                               */

static int
cache_tables_create(sqlite3 *hdl, int version, struct cache_db_def *db_def, int db_def_size)
{
#define Q_CACHE_VERSION "INSERT INTO admin (key, value) VALUES ('cache_version', '%d');"
  char *query;
  char *errmsg;
  int ret;
  int i;

  for (i = 0; i < db_def_size; i++)
    {
      ret = sqlite3_exec(hdl, db_def[i].create_query, NULL, NULL, &errmsg);
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_FATAL, L_CACHE, "Error creating cache db entity '%s': %s\n", db_def[i].name, errmsg);
	  sqlite3_free(errmsg);
	  return -1;
	}
    }

  query = sqlite3_mprintf(Q_CACHE_VERSION, version);
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  sqlite3_free(query);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_FATAL, L_CACHE, "Error inserting cache version: %s\n", errmsg);
      sqlite3_free(errmsg);
      return -1;
    }

  return 0;
#undef Q_CACHE_VERSION
}

static int
cache_tables_drop(sqlite3 *hdl, struct cache_db_def *db_def, int db_def_size)
{
#define Q_VACUUM	"VACUUM;"
  char *errmsg;
  int ret;
  int i;

  for (i = 0; i < db_def_size; i++)
    {
      ret = sqlite3_exec(hdl, db_def[i].drop_query, NULL, NULL, &errmsg);
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_FATAL, L_CACHE, "Error dropping cache db entity '%s': %s\n", db_def[i].name, errmsg);
	  sqlite3_free(errmsg);
	  return -1;
	}
    }

  ret = sqlite3_exec(hdl, Q_VACUUM, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_CACHE, "Error vacuuming cache database: %s\n", errmsg);
      sqlite3_free(errmsg);
      return -1;
    }

  return 0;
#undef Q_VACUUM
}

static int
cache_version_check(int *have_version, sqlite3 *hdl, int want_version)
{
#define Q_VER "SELECT value FROM admin WHERE key = 'cache_version';"
  sqlite3_stmt *stmt;
  int ret;

  *have_version = 0;

  ret = sqlite3_prepare_v2(hdl, Q_VER, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      return 0; // Virgin database, admin table doesn't exists
    }

  ret = sqlite3_step(stmt);
  if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_CACHE, "Could not step: %s\n", sqlite3_errmsg(hdl));
      sqlite3_finalize(stmt);
      return -1;
    }

  *have_version = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  return 0;
#undef Q_VER
}

static int
cache_pragma_set(sqlite3 *hdl)
{
#define Q_PRAGMA_CACHE_SIZE "PRAGMA cache_size=%d;"
#define Q_PRAGMA_JOURNAL_MODE "PRAGMA journal_mode=%s;"
#define Q_PRAGMA_SYNCHRONOUS "PRAGMA synchronous=%d;"
#define Q_PRAGMA_MMAP_SIZE "PRAGMA mmap_size=%d;"
  char *errmsg;
  int ret;
  int cache_size;
  char *journal_mode;
  int synchronous;
  int mmap_size;
  char *query;

  // Set page cache size in number of pages
  cache_size = config_get_int("pragma_cache_size_cache", -1);
  if (cache_size > -1)
    {
      query = sqlite3_mprintf(Q_PRAGMA_CACHE_SIZE, cache_size);
      ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
      sqlite3_free(query);
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_LOG, L_CACHE, "Error setting pragma_cache_size_cache: %s\n", errmsg);

	  sqlite3_free(errmsg);
	  return -1;
	}
    }

  // Set journal mode
  journal_mode = config_get_str("pragma_journal_mode", NULL);
  if (journal_mode)
    {
      query = sqlite3_mprintf(Q_PRAGMA_JOURNAL_MODE, journal_mode);
      ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
      sqlite3_free(query);
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_LOG, L_CACHE, "Error setting pragma_journal_mode: %s\n", errmsg);

	  sqlite3_free(errmsg);
	  return -1;
	}
    }

  // Set synchronous flag
  synchronous = config_get_int("pragma_synchronous", -1);
  if (synchronous > -1)
    {
      query = sqlite3_mprintf(Q_PRAGMA_SYNCHRONOUS, synchronous);
      ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
      sqlite3_free(query);
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_LOG, L_CACHE, "Error setting pragma_synchronous: %s\n", errmsg);

	  sqlite3_free(errmsg);
	  return -1;
	}
    }

  // Set mmap size
  mmap_size = config_get_int("pragma_mmap_size_cache", -1);
  if (synchronous > -1)
    {
      query = sqlite3_mprintf(Q_PRAGMA_MMAP_SIZE, mmap_size);
      ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_LOG, L_CACHE, "Error setting pragma_mmap_size: %s\n", errmsg);

	  sqlite3_free(errmsg);
	  return -1;
	}
    }

  return 0;
#undef Q_PRAGMA_CACHE_SIZE
#undef Q_PRAGMA_JOURNAL_MODE
#undef Q_PRAGMA_SYNCHRONOUS
#undef Q_PRAGMA_MMAP_SIZE
}

static void
cache_close_one(sqlite3 **hdl)
{
  sqlite3_stmt *stmt;

  if (!*hdl)
    return;

  /* Tear down anything that's in flight */
  while ((stmt = sqlite3_next_stmt(*hdl, 0)))
    sqlite3_finalize(stmt);

  sqlite3_close(*hdl);
  *hdl = NULL;
}

static void
cache_close(void)
{
  cache_close_one(&cache_xcode_hdl);

  DPRINTF(E_DBG, L_CACHE, "Cache closed\n");
}

static int
cache_open_one(sqlite3 **hdl, const char *path, const char *name, int want_version, struct cache_db_def *db_def, int db_def_size)
{
  sqlite3 *h;
  int have_version;
  int ret;

  ret = sqlite3_open(path, &h);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_CACHE, "Could not open '%s': %s\n", path, sqlite3_errmsg(h));
      goto error;
    }

  ret = cache_version_check(&have_version, h, want_version);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_CACHE, "Could not check cache '%s' database version\n", name);
      goto error;
    }

  if (have_version > 0 && have_version < want_version)
    {
      DPRINTF(E_LOG, L_CACHE, "Database schema outdated, deleting cache '%s' v%d -> v%d\n", name, have_version, want_version);

      ret = cache_tables_drop(h, db_def, db_def_size);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_CACHE, "Error deleting '%s' database tables\n", name);
	  goto error;
	}
    }

  if (have_version < want_version)
    {
      ret = cache_tables_create(h, want_version, db_def, db_def_size);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_CACHE, "Could not create cache '%s' database tables\n", name);
	  goto error;
	}

      DPRINTF(E_INFO, L_CACHE, "Cache '%s' database tables created\n", name);
    }

  *hdl = h;
  return 0;

 error:
  sqlite3_close(h);
  return -1;
}

static int
cache_open(void)
{
  const char *directory;
  const char *filename;
  char *xcode_db_path;
  int ret;

  directory = config_get_str("cache_dir", STATEDIR "/cache/" PACKAGE "/");

  CHECK_NULL(L_DB, filename = config_get_str("cache_xcode_filename", "cache.db"));
  CHECK_NULL(L_DB, xcode_db_path = safe_asprintf("%s%s", directory, filename));

  ret = cache_open_one(&cache_xcode_hdl, xcode_db_path, "xcode", CACHE_XCODE_VERSION, cache_xcode_db_def, ARRAY_SIZE(cache_xcode_db_def));
  if (ret < 0)
    goto error;

  ret = cache_pragma_set(cache_xcode_hdl);
  if (ret < 0)
    goto error;

  DPRINTF(E_DBG, L_CACHE, "Cache opened\n");

  free(xcode_db_path);
  return 0;

 error:
  cache_close();
  free(xcode_db_path);
  return -1;
}


/* ----------------------- Caching of transcoded data ----------------------- */

static void
xcode_job_clear(struct cache_xcode_job *job)
{
  free(job->file_path);
  if (job->header)
    evbuffer_free(job->header);

  // Can't just memset to zero, because *ev is persistent
  job->format = NULL;
  job->file_path = NULL;
  job->file_id = 0;
  job->header = NULL;
  job->is_encoding = false;
}

static enum command_state
xcode_header_get(void *arg, int *retval)
{
#define Q_TMPL "SELECT header FROM data WHERE length(header) > 0 AND file_id = ? AND format = ?;"
  struct cache_arg *cmdarg = arg;
  sqlite3_stmt *stmt = NULL;
  int ret;

  cmdarg->cached = 0;

  ret = sqlite3_prepare_v2(cmdarg->hdl, Q_TMPL, -1, &stmt, 0);
  if (ret != SQLITE_OK)
    goto error;

  sqlite3_bind_int(stmt, 1, cmdarg->id);
  sqlite3_bind_text(stmt, 2, cmdarg->header_format, -1, SQLITE_STATIC);

  ret = sqlite3_step(stmt);
  if (ret == SQLITE_DONE)
    goto end;
  else if (ret != SQLITE_ROW)
    goto error;

  ret = evbuffer_add(cmdarg->evbuf, sqlite3_column_blob(stmt, 0), sqlite3_column_bytes(stmt, 0));
  if (ret < 0)
    goto error;

  cmdarg->cached = 1;

  DPRINTF(E_DBG, L_CACHE, "Cache header hit (%zu bytes)\n", evbuffer_get_length(cmdarg->evbuf));

 end:
  sqlite3_finalize(stmt);
  *retval = 0;
  return COMMAND_END;

 error:
  DPRINTF(E_LOG, L_CACHE, "Database error getting prepared header from cache: %s\n", sqlite3_errmsg(cmdarg->hdl));
  if (stmt)
    sqlite3_finalize(stmt);
  *retval = -1;
  return COMMAND_END;
#undef Q_TMPL
}

static void
xcode_trigger(void)
{
  struct timeval delay_xcode = { 5, 0 };

  if (cache_xcode_is_enabled)
    event_add(cache_xcode_updateev, &delay_xcode);
}

static enum command_state
xcode_toggle(void *arg, int *retval)
{
  bool *enable = arg;

  if (*enable == cache_xcode_is_enabled)
    goto end;

  cache_xcode_is_enabled = *enable;
  xcode_trigger();

 end:
  *retval = 0;
  return COMMAND_END;
}

static int
xcode_add_entry(sqlite3 *hdl, uint32_t id, uint32_t ts, const char *path)
{
#define Q_TMPL "INSERT OR REPLACE INTO files (id, time_modified, filepath) VALUES (%d, %d, '%q');"
  char *query;
  char *errmsg;
  int ret;

  DPRINTF(E_SPAM, L_CACHE, "Adding xcode file id %d, path '%s'\n", id, path);

  query = sqlite3_mprintf(Q_TMPL, id, ts, path);

  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  sqlite3_free(query);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_CACHE, "Error adding row to cache: %s\n", errmsg);
      sqlite3_free(errmsg);
      return -1;
    }

  return 0;
#undef Q_TMPL
}

static int
xcode_del_entry(sqlite3 *hdl, uint32_t id)
{
#define Q_TMPL_FILES "DELETE FROM files WHERE id = %d;"
#define Q_TMPL_DATA "DELETE FROM data WHERE file_id = %d;"
  char query[256];
  char *errmsg;
  int ret;

  DPRINTF(E_SPAM, L_CACHE, "Deleting xcode file id %d\n", id);

  sqlite3_snprintf(sizeof(query), query, Q_TMPL_FILES, (int)id);
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_CACHE, "Error deleting row from xcode files: %s\n", errmsg);
      sqlite3_free(errmsg);
      return -1;
    }

  sqlite3_snprintf(sizeof(query), query, Q_TMPL_DATA, (int)id);
  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_CACHE, "Error deleting rows from xcode_data: %s\n", errmsg);
      sqlite3_free(errmsg);
      return -1;
    }

  return 0;
#undef Q_TMPL_DATA
#undef Q_TMPL_FILES
}

/* In the xcode table we keep a prepared header for files that could be subject
 * to transcoding. Whenever the library changes, this callback runs, and the
 * list of files in the xcode table is synced with the main files table.
 *
 * In practice we compare two tables, both sorted by id:
 *
 * From files:                         From the cache
 *  | id      |  time_modified  |       | id       | time_modified | data    |
 *
 * We do it one item at the time from files, and then going through cache table
 * rows until: table end OR id is larger OR id is equal and time equal or newer
 */
static int
xcode_sync_with_files(sqlite3 *hdl)
{
  sqlite3_stmt *stmt;
  struct cachelist *cachelist = NULL;
  size_t cachelist_size = 0;
  size_t cachelist_len = 0;
  struct query_params qp = { .type = Q_ITEMS, .filter = "f.data_kind = 0", .order = "f.id" };
  struct db_media_file_info dbmfi;
  struct db_media_file_info *rowA;
  struct cachelist *rowB;
  uint32_t id;
  uint32_t ts;
  int cmp;
  int i;
  int ret;

  // Both lists must be sorted by id, otherwise the compare below won't work
  ret = sqlite3_prepare_v2(hdl, "SELECT id, time_modified FROM files ORDER BY id;", -1, &stmt, 0);
  if (ret != SQLITE_OK)
    goto error;

  while (sqlite3_step(stmt) == SQLITE_ROW)
    {
      if (cachelist_len + 1 > cachelist_size)
	{
	  cachelist_size += 1024;
	  CHECK_NULL(L_CACHE, cachelist = realloc(cachelist, cachelist_size * sizeof(struct cachelist)));
	}
      cachelist[cachelist_len].id = sqlite3_column_int(stmt, 0);
      cachelist[cachelist_len].ts = sqlite3_column_int(stmt, 1);
      cachelist_len++;
    }
  sqlite3_finalize(stmt);

  ret = db_query_start(&qp);
  if (ret < 0)
    goto error;

  // Silence false maybe-uninitialized warning
  rowB = NULL;

  // Loop while either list ("A" files list, "B" cache list) has remaining items
  for(i = 0, cmp = 0;;)
    {
      if (cmp <= 0)
        rowA = (db_query_fetch_file(&dbmfi, &qp) == 0) ? &dbmfi : NULL;;
      if (cmp >= 0)
        rowB = (i < cachelist_len) ? &cachelist[i++] : NULL;
      if (!rowA && !rowB)
        break; // Done with both lists

#if 0
      if (rowA)
	DPRINTF(E_DBG, L_CACHE, "cmp %d, rowA->id %s\n", cmp, rowA->id);
      if (rowB)
	DPRINTF(E_DBG, L_CACHE, "cmp %d, rowB->id %u, i %d, cachelist_len %zu\n", cmp, rowB->id, i, cachelist_len);
#endif

      if (rowA)
	{
	  safe_atou32(rowA->id, &id);
	  safe_atou32(rowA->time_modified, &ts);
	}

      cmp = 0; // In both lists - unless:
      if (!rowB || (rowA && rowB->id > id)) // A had an item not in B
	{
	  xcode_add_entry(hdl, id, ts, rowA->path);
	  cmp = -1;
	}
      else if (!rowA || (rowB && rowB->id < id)) // B had an item not in A
	{
	  xcode_del_entry(hdl, rowB->id);
	  cmp = 1;
	}
      else if (rowB->id == id && rowB->ts < ts) // Item in B is too old
	{
	  xcode_del_entry(hdl, rowB->id);
	  xcode_add_entry(hdl, id, ts, rowA->path);
	}
    }

  db_query_end(&qp);

  free(cachelist);
  return 0;

 error:
  DPRINTF(E_LOG, L_CACHE, "Database error while processing xcode files table\n");
  free(cachelist);
  return -1;
}

static int
xcode_header_save(sqlite3 *hdl, int file_id, const char *format, uint8_t *data, size_t datalen)
{
#define Q_TMPL "INSERT INTO data (timestamp, file_id, format, header) VALUES (?, ?, ?, ?);"
  sqlite3_stmt *stmt;
  int ret;

  ret = sqlite3_prepare_v2(hdl, Q_TMPL, -1, &stmt, 0);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_CACHE, "Error preparing xcode data for cache update: %s\n", sqlite3_errmsg(hdl));
      return -1;
    }

  sqlite3_bind_int(stmt, 1, (uint64_t)time(NULL));
  sqlite3_bind_int(stmt, 2, file_id);
  sqlite3_bind_text(stmt, 3, format, -1, SQLITE_STATIC);
  sqlite3_bind_blob(stmt, 4, data, datalen, SQLITE_STATIC);

  ret = sqlite3_step(stmt);
  if (ret != SQLITE_DONE)
    {
      DPRINTF(E_LOG, L_CACHE, "Error stepping xcode data for cache update: %s\n", sqlite3_errmsg(hdl));
      return -1;
    }

  sqlite3_finalize(stmt);
  return 0;
#undef Q_TMPL
}

static int
xcode_file_next(int *file_id, char **file_path, sqlite3 *hdl, const char *format)
{
#define Q_TMPL "SELECT f.id, f.filepath, d.id FROM files f LEFT JOIN data d ON f.id = d.file_id AND d.format = '%q' WHERE d.id IS NULL LIMIT 1;"
  sqlite3_stmt *stmt;
  char query[256];
  int ret;

  sqlite3_snprintf(sizeof(query), query, Q_TMPL, format);

  ret = sqlite3_prepare_v2(hdl, query, -1, &stmt, 0);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_CACHE, "Error occured while finding next file to prepare header for\n");
      return -1;
    }

  ret = sqlite3_step(stmt);
  if (ret != SQLITE_ROW)
    {
      sqlite3_finalize(stmt);
      return -1; // All done
    }

  *file_id = sqlite3_column_int(stmt, 0);
  *file_path = strdup((char *)sqlite3_column_text(stmt, 1));

  sqlite3_finalize(stmt);

  // Save an empty header so next call to this function will return a new file
  return xcode_header_save(hdl, *file_id, format, NULL, 0);
#undef Q_TMPL
}

// Thread: worker
static void
xcode_worker(void *arg)
{
  struct cache_xcode_job *job = *(struct cache_xcode_job **)arg;
  int ret;

  DPRINTF(E_DBG, L_CACHE, "Preparing %s header for '%s' (file id %d)\n", job->format, job->file_path, job->file_id);

  if (strcmp(job->format, CACHE_XCODE_FORMAT_MP4) == 0)
    {
      ret = transcode_prepare_header(&job->header, XCODE_MP4_ALAC, job->file_path);
      if (ret < 0)
	DPRINTF(E_LOG, L_CACHE, "Error preparing %s header for '%s' (file id %d)\n", job->format, job->file_path, job->file_id);
    }

  // Tell the cache thread that we are done. Only the cache thread can save the
  // result to the DB.
  event_active(job->ev, 0, 0);
}

static void
cache_xcode_job_complete_cb(int fd, short what, void *arg)
{
  struct cache_xcode_job *job = arg;
  uint8_t *data;
  size_t datalen;

  if (job->header)
    {
#if 1
      datalen = evbuffer_get_length(job->header);
      data = evbuffer_pullup(job->header, -1);
#else
      data = (unsigned char*)"dummy";
      datalen = 6;
#endif
      xcode_header_save(cache_xcode_hdl, job->file_id, job->format, data, datalen);
    }

  xcode_job_clear(job); // Makes the job available again
  event_active(cache_xcode_prepareev, 0, 0);
}

// Preparing headers can take very long, so we use worker threads. However, all
// DB access must be from the cache thread. So this function will find the next
// file from the db and then dispatch a thread for the encoding.
static void
cache_xcode_prepare_cb(int fd, short what, void *arg)
{
  struct cache_xcode_job *job = NULL;
  bool is_encoding = false;
  int ret;
  int i;

  if (!cache_is_initialized)
    return;

  for (i = 0; i < ARRAY_SIZE(cache_xcode_jobs); i++)
    {
      if (cache_xcode_jobs[i].is_encoding)
	is_encoding = true;
      else if (!job)
	job = &cache_xcode_jobs[i];
    }

  if (!job)
    return; // No available thread right now, wait for cache_xcode_job_complete_cb()

  ret = xcode_file_next(&job->file_id, &job->file_path, cache_xcode_hdl, CACHE_XCODE_FORMAT_MP4);
  if (ret < 0)
    {
      if (!is_encoding)
	DPRINTF(E_LOG, L_CACHE, "Header generation completed\n");

      return;
    }
  else if (!is_encoding)
    DPRINTF(E_LOG, L_CACHE, "Kicking off header generation\n");

  job->is_encoding = true;
  job->format = CACHE_XCODE_FORMAT_MP4;

  worker_execute(xcode_worker, &job, sizeof(struct cache_xcode_job *), 0);

  // Set off more threads
  event_active(cache_xcode_prepareev, 0, 0);
}

static void
cache_xcode_update_cb(int fd, short what, void *arg)
{
  if (xcode_sync_with_files(cache_xcode_hdl) < 0)
    return;

  event_active(cache_xcode_prepareev, 0, 0);
}

/* Sets off an update by activating the event. The delay is because we are low
 * priority compared to other listeners of database updates.
 */
static enum command_state
cache_database_update(void *arg, int *retval)
{
  xcode_trigger();

  *retval = 0;
  return COMMAND_END;
}

/* Callback from database listener */
static void
cache_listener_cb(short event_mask, void *ctx)
{
  commands_exec_async(cmdbase, cache_database_update, NULL);
}


static void *
cache(void *arg)
{
  int ret;
  int i;

  thread_setname("cache");

  ret = cache_open();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_CACHE, "Error: Cache create failed. Cache will be disabled.\n");
      pthread_exit(NULL);
    }

  ret = db_perthread_init();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_CACHE, "Error: DB init failed. Cache will be disabled.\n");
      cache_close();

      pthread_exit(NULL);
    }

  CHECK_NULL(L_CACHE, cache_xcode_updateev = evtimer_new(evbase_cache, cache_xcode_update_cb, NULL));
  CHECK_NULL(L_CACHE, cache_xcode_prepareev = evtimer_new(evbase_cache, cache_xcode_prepare_cb, NULL));
  CHECK_ERR(L_CACHE, event_priority_set(cache_xcode_prepareev, 0));
  for (i = 0; i < ARRAY_SIZE(cache_xcode_jobs); i++)
    CHECK_NULL(L_CACHE, cache_xcode_jobs[i].ev = evtimer_new(evbase_cache, cache_xcode_job_complete_cb, &cache_xcode_jobs[i]));

  CHECK_ERR(L_CACHE, listener_add(cache_listener_cb, LISTENER_DATABASE, NULL));

  cache_is_initialized = 1;

  event_base_dispatch(evbase_cache);

  if (cache_is_initialized)
    {
      DPRINTF(E_LOG, L_CACHE, "Cache event loop terminated ahead of time!\n");
      cache_is_initialized = 0;
    }

  listener_remove(cache_listener_cb);

  for (i = 0; i < ARRAY_SIZE(cache_xcode_jobs); i++)
    event_free(cache_xcode_jobs[i].ev);
  event_free(cache_xcode_prepareev);
  event_free(cache_xcode_updateev);

  db_perthread_deinit();

  cache_close();

  pthread_exit(NULL);
}


/* --------------------------- Transcode cache API  ------------------------- */

int
cache_xcode_header_get(struct evbuffer *evbuf, int *cached, uint32_t id, const char *format)
{
  struct cache_arg cmdarg;
  int ret;

  if (!cache_is_initialized)
    return -1;

  cmdarg.hdl = cache_xcode_hdl;
  cmdarg.evbuf = evbuf;
  cmdarg.id = id;
  cmdarg.header_format = format;

  ret = commands_exec_sync(cmdbase, xcode_header_get, NULL, &cmdarg);

  *cached = cmdarg.cached;

  return ret;
}

int
cache_xcode_toggle(bool enable)
{
  if (!cache_is_initialized)
    return -1;

  return commands_exec_sync(cmdbase, xcode_toggle, NULL, &enable);
}


/* --------------------------- Cache general API ---------------------------- */

int
cache_init(void)
{
  CHECK_NULL(L_CACHE, evbase_cache = event_base_new());
  CHECK_ERR(L_CACHE, event_base_priority_init(evbase_cache, 8));
  CHECK_NULL(L_CACHE, cmdbase = commands_base_new(evbase_cache, NULL));

  CHECK_ERR(L_CACHE, pthread_create(&tid_cache, NULL, cache, NULL));

  DPRINTF(E_INFO, L_CACHE, "Cache thread init\n");

  return 0;
}

void
cache_deinit(void)
{
  int ret;

  if (!cache_is_initialized)
    return;

  cache_is_initialized = 0;

  commands_base_destroy(cmdbase);

  ret = pthread_join(tid_cache, NULL);
  if (ret != 0)
    {
      DPRINTF(E_FATAL, L_CACHE, "Could not join cache thread: %s\n", strerror(errno));
      return;
    }

  event_base_free(evbase_cache);
}
