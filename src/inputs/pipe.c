/*
 * Copyright (C) 2017 Espen Jurgensen
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
 * About pipe.c
 * --------------
 * This module will read a PCM16 stream from a named pipe and write it to the
 * input buffer. The user may start/stop playback from a pipe by selecting it
 * through a client. If the user has configured pipe_autostart, then pipes in
 * the library will also be watched for data, and playback will start/stop
 * automatically.
 *
 * The module will also look for pipes with a .metadata suffix, and if found,
 * the metadata will be parsed and fed to the player. The metadata must be in
 * the format Shairport uses for this purpose.
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <event2/event.h>
#include <event2/buffer.h>

#include "input.h"
#include "misc.h"
#include "logger.h"
#include "owntone_config.h"
#include "player.h"
#include "worker.h"
#include "commands.h"

// Maximum number of pipes to watch for data
#define PIPE_MAX_WATCH 4
// How often to check that our watches are still attached to the fifo that the
// path points at, and to retry watches that could not be attached
#define PIPE_WATCH_HEALTH_INTERVAL 30
// Max number of bytes to read from a pipe at a time
#define PIPE_READ_MAX 65536
// Max number of bytes to buffer from metadata pipes
#define PIPE_METADATA_BUFLEN_MAX 1048576
// Ignore pictures with larger size than this
#define PIPE_PICTURE_SIZE_MAX 1048576
// Where we store pictures for the artwork module to read
#define PIPE_TMPFILE_TEMPLATE "/tmp/" PACKAGE_NAME ".XXXXXX.ext"
#define PIPE_TMPFILE_TEMPLATE_EXTLEN 4

enum pipetype
{
  PIPE_PCM,
  PIPE_METADATA,
};

enum pipe_metadata_msg
{
  PIPE_METADATA_MSG_METADATA = (1 << 0),
  PIPE_METADATA_MSG_PROGRESS = (1 << 1),
  PIPE_METADATA_MSG_VOLUME   = (1 << 2),
  PIPE_METADATA_MSG_PICTURE  = (1 << 3),
  PIPE_METADATA_MSG_FLUSH    = (1 << 4),
};

struct pipe
{
  int id;               // The mfi id of the pipe
  int fd;               // File descriptor
  bool is_autostarted;  // We autostarted the pipe (and we will autostop)
  bool watch_failed;    // Reattaching the watch failed, don't log repeat attempts
  char *path;           // Path
  enum pipetype type;   // PCM (audio) or metadata
  event_callback_fn cb; // Callback when there is data to read
  struct event *ev;     // Event for the callback

  struct pipe *next;
};

// struct for storing the data received via a metadata pipe
struct pipe_metadata_prepared
{
  // Progress, artist etc goes here
  struct input_metadata input_metadata;
  // Picture (artwork) data
  int pict_tmpfile_fd;
  char pict_tmpfile_path[sizeof(PIPE_TMPFILE_TEMPLATE)];
  // The tmpfile from the previous picture, kept on disk one generation longer
  // than the fd -- see pict_tmpfile_recreate(). Empty when there is none.
  char pict_tmpfile_path_prev[sizeof(PIPE_TMPFILE_TEMPLATE)];
  // Volume
  int volume;
  // Mutex to share the prepared metadata
  pthread_mutex_t lock;
};

// Extension of struct pipe with extra fields for metadata handling
struct pipe_metadata
{
  // Pipe that we start watching for metadata after playback starts
  struct pipe *pipe;
  // We read metadata into this evbuffer
  struct evbuffer *evbuf;
  // Storage of current metadata
  struct pipe_metadata_prepared prepared;
  // True if there is new metadata to push to the player
  bool is_new;
  // .pipe and .evbuf are set up/torn down by pipe_metadata_watch_add()/
  // pipe_metadata_watch_del(), which run as jobs on the (multi-threaded)
  // worker pool, and are also read from the pipe thread in
  // pipe_metadata_read_cb() and from the input thread in stop(). This mutex
  // protects .pipe and .evbuf specifically (not .prepared, which has its own
  // lock above).
  pthread_mutex_t lock;
};

union pipe_arg
{
  uint32_t id;
  struct pipe *pipelist;
};

// The usual thread stuff
static pthread_t tid_pipe;
static struct event_base *evbase_pipe;
static struct commands_base *cmdbase;

// From config - the sample rate and bps of the pipe input
static int pipe_sample_rate;
static int pipe_bits_per_sample;
// From config - path to the named pipe/FIFO
static char *pipe_path;
// From config - should we watch the pipe for data or only start on request
static int pipe_autostart;
// The synthetic id used for the config-driven pipe (fixed)
static int pipe_autostart_id;

// Timer that keeps the watches in pipe_watch_list attached
static struct event *pipe_watch_health_ev;

// Set (on the pipe thread) once shutdown has begun: all watches are disarmed
// and must stay disarmed, so that no autostart can fire into a player that is
// being torn down. Read only on the pipe thread.
static bool pipe_quiesced;

// Global list of pipes we are watching (if watching/autostart is enabled)
static struct pipe *pipe_watch_list;

// Pipe + extra fields that we start watching for metadata after playback starts
static struct pipe_metadata pipe_metadata;

/* -------------------------------- HELPERS --------------------------------- */

// These might be more at home in dmap_common.c
static inline uint32_t
dmap_str2val(const char s[4])
{
  return ((s[0] << 24) | (s[1] << 16) | (s[2] << 8) | (s[3] << 0));
}

static void
dmap_val2str(char buf[5], uint32_t val)
{
  buf[0] = (val >> 24) & 0xff;
  buf[1] = (val >> 16) & 0xff;
  buf[2] = (val >> 8) & 0xff;
  buf[3] = val & 0xff;
  buf[4] = 0;
}

static struct pipe *
pipe_create(const char *path, int id, enum pipetype type, event_callback_fn cb)
{
  struct pipe *pipe;

  CHECK_NULL(L_PLAYER, pipe = calloc(1, sizeof(struct pipe)));
  pipe->path  = strdup(path);
  pipe->id    = id;
  pipe->fd    = -1;
  pipe->type  = type;
  pipe->cb    = cb;

  return pipe;
}

static void
pipe_free(struct pipe *pipe)
{
  free(pipe->path);
  free(pipe);
}

static int
pipe_open(const char *path, bool silent)
{
  struct stat sb;
  int fd;

  DPRINTF(E_DBG, L_PLAYER, "(Re)opening pipe: '%s'\n", path);

  fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      if (!silent)
	DPRINTF(E_LOG, L_PLAYER, "Could not open pipe for reading '%s': %s\n", path, strerror(errno));
      goto error;
    }

  if (fstat(fd, &sb) < 0)
    {
      if (!silent)
	DPRINTF(E_LOG, L_PLAYER, "Could not fstat() '%s': %s\n", path, strerror(errno));
      goto error;
    }

  if (!S_ISFIFO(sb.st_mode))
    {
      DPRINTF(E_LOG, L_PLAYER, "Source type is pipe, but path is not a fifo: %s\n", path);
      goto error;
    }

  return fd;

 error:
  if (fd >= 0)
    close(fd);

  return -1;
}

static void
pipe_close(int fd)
{
  if (fd >= 0)
    close(fd);
}

static int
watch_add(struct pipe *pipe)
{
  bool silent;

  if (pipe_quiesced)
    return -1;

  silent = (pipe->type == PIPE_METADATA) || pipe->watch_failed;
  pipe->fd = pipe_open(pipe->path, silent);
  if (pipe->fd < 0)
    return -1;

  pipe->ev = event_new(evbase_pipe, pipe->fd, EV_READ, pipe->cb, pipe);
  if (!pipe->ev)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not watch pipe for new data '%s'\n", pipe->path);
      pipe_close(pipe->fd);
      return -1;
    }

  event_add(pipe->ev, NULL);

  return 0;
}

// Idempotent: a watch can legitimately be deleted twice - the quiesce pass
// disarms every watch, but a watch-update command already queued behind it
// still runs its own removal afterwards - so the freed event must not stay
// dangling.
static void
watch_del(struct pipe *pipe)
{
  if (pipe->ev)
    {
      event_free(pipe->ev);
      pipe->ev = NULL;
    }

  pipe_close(pipe->fd);

  pipe->fd = -1;
}

// If a read on pipe returns 0 it is an EOF, and we must close it and reopen it
// for renewed watching. The event will be freed and reallocated by this.
static int
watch_reset(struct pipe *pipe)
{
  if (!pipe)
    return -1;

  watch_del(pipe);

  return watch_add(pipe);
}

// A watch stops working, without any error being reported, if the fifo it was
// opened on is deleted and then recreated by someone else: our fd stays valid,
// but it refers to the old, unlinked fifo, so data written to the new one never
// wakes us. Comparing the fd to the path detects that, and also covers a watch
// we never managed to attach because the fifo did not exist yet.
static bool
watch_is_stale(struct pipe *pipe)
{
  struct stat sb_path;
  struct stat sb_fd;

  if (pipe->fd < 0)
    return true;

  if (stat(pipe->path, &sb_path) < 0)
    return true;

  if (fstat(pipe->fd, &sb_fd) < 0)
    return true;

  return (sb_path.st_ino != sb_fd.st_ino) || (sb_path.st_dev != sb_fd.st_dev);
}

static void
pipelist_add(struct pipe **list, struct pipe *pipe)
{
  pipe->next = *list;
  *list = pipe;
}

static void
pipelist_remove(struct pipe **list, struct pipe *pipe)
{
  struct pipe *prev = NULL;
  struct pipe *p;

  for (p = *list; p; p = p->next)
    {
      if (p->id == pipe->id)
	break;

      prev = p;
    }

  if (!p)
    return;

  if (!prev)
    *list = pipe->next;
  else
    prev->next = pipe->next;

  pipe_free(pipe);
}

static struct pipe *
pipelist_find(struct pipe *list, int id)
{
  struct pipe *p;

  for (p = list; p; p = p->next)
    {
      if (id == p->id)
	return p;
    }

  return NULL;
}

static void
pict_tmpfile_close(int fd, const char *path)
{
  if (fd < 0)
    return;

  close(fd);
  unlink(path);
}

// Removes both the current artwork tmpfile and the retained previous one.
// Used when metadata handling stops, so nothing is left behind in /tmp.
static void
pict_tmpfiles_remove(struct pipe_metadata_prepared *prepared)
{
  pict_tmpfile_close(prepared->pict_tmpfile_fd, prepared->pict_tmpfile_path);
  prepared->pict_tmpfile_fd = -1;
  prepared->pict_tmpfile_path[0] = '\0';

  if (prepared->pict_tmpfile_path_prev[0])
    {
      unlink(prepared->pict_tmpfile_path_prev);
      prepared->pict_tmpfile_path_prev[0] = '\0';
    }
}

// Opens a tmpfile to store metadata artwork in. *ext is the extension to use
// for the tmpfile, eg .jpg or .png. Extension cannot be longer than
// PIPE_TMPFILE_TEMPLATE_EXTLEN. The path buffer in *prepared is updated with
// the new tmpfile, and the fd is returned.
//
// The outgoing tmpfile is closed but deliberately left on disk for one more
// picture. Its path stays published as the queue item's artwork_url, and a
// client can still be reading it when the next picture arrives -- deleting it
// at that moment makes the read fail with ENOENT. Retiring it one generation
// later instead gives readers the life of the following picture to finish. At
// most two files exist at a time, and both are removed when metadata handling
// stops.
static int
pict_tmpfile_recreate(struct pipe_metadata_prepared *prepared, const char *ext)
{
  char *path = prepared->pict_tmpfile_path;
  char *path_prev = prepared->pict_tmpfile_path_prev;
  int offset = strlen(PIPE_TMPFILE_TEMPLATE) - PIPE_TMPFILE_TEMPLATE_EXTLEN;

  if (strlen(ext) > PIPE_TMPFILE_TEMPLATE_EXTLEN)
    {
      DPRINTF(E_LOG, L_PLAYER, "Invalid extension provided to pict_tmpfile_recreate: '%s'\n", ext);
      return -1;
    }

  // Retire the picture before last; anything reading it has had a full
  // picture's worth of time to do so.
  if (path_prev[0])
    unlink(path_prev);

  // Close the outgoing file but leave it on disk for one more generation.
  if (prepared->pict_tmpfile_fd >= 0)
    close(prepared->pict_tmpfile_fd);
  prepared->pict_tmpfile_fd = -1;

  memcpy(path_prev, path, sizeof(prepared->pict_tmpfile_path_prev));

  strcpy(path, PIPE_TMPFILE_TEMPLATE);
  strcpy(path + offset, ext);

  return mkstemps(path, PIPE_TMPFILE_TEMPLATE_EXTLEN);
}

static int
parse_progress(struct pipe_metadata_prepared *prepared, char *progress)
{
  struct input_metadata *m = &prepared->input_metadata;
  char *s;
  char *ptr;
  // Below must be signed to avoid casting in the calculations of pos_ms/len_ms
  int64_t start;
  int64_t pos;
  int64_t end;

  if (!(s = strtok_r(progress, "/", &ptr)))
    goto error;
  safe_atoi64(s, &start);

  if (!(s = strtok_r(NULL, "/", &ptr)))
    goto error;
  safe_atoi64(s, &pos);

  if (!(s = strtok_r(NULL, "/", &ptr)))
    goto error;
  safe_atoi64(s, &end);

  if (!start || !pos || !end)
    goto error;

  // Note that negative positions are allowed and supported. A negative position
  // of e.g. -1000 means that the track will start in one second.
  m->pos_is_updated = true;
  m->pos_ms = (pos - start) * 1000 / pipe_sample_rate;
  m->len_ms = (end > start) ? (end - start) * 1000 / pipe_sample_rate : 0;

  DPRINTF(E_DBG, L_PLAYER, "Received Shairport metadata progress: %" PRIi64 "/%" PRIi64 "/%" PRIi64 " => %d/%u ms\n", start, pos, end, m->pos_ms, m->len_ms);

  return 0;

 error:
  DPRINTF(E_LOG, L_PLAYER, "Received unexpected Shairport metadata progress: %s\n", progress);
  return -1;
}

static int
parse_volume(struct pipe_metadata_prepared *prepared, const char *volume)
{
  char *volume_next;
  float airplay_volume;
  int local_volume;

  errno = 0;
  airplay_volume = strtof(volume, &volume_next);

  if ((errno == ERANGE) || (volume == volume_next))
    {
      DPRINTF(E_LOG, L_PLAYER, "Invalid Shairport airplay volume in string (%s): %s\n", volume,
	      (errno == ERANGE ? strerror(errno) : "First token is not a number."));
      goto error;
    }

  if (strcmp(volume_next, ",0.00,0.00,0.00") != 0)
    {
      DPRINTF(E_DBG, L_PLAYER, "Not applying Shairport airplay volume while software volume control is enabled (%s)\n", volume);
      goto error; // Not strictly an error but goes through same flow
    }

  if (((int) airplay_volume) == -144)
    {
      DPRINTF(E_DBG, L_PLAYER, "Applying Shairport airplay volume ('mute', value: %.2f)\n", airplay_volume);
      prepared->volume = 0;
    }
  else if (airplay_volume >= -30.0 && airplay_volume <= 0.0)
    {
      local_volume = (int)(100.0 + (airplay_volume / 30.0 * 100.0));

      DPRINTF(E_DBG, L_PLAYER, "Applying Shairport airplay volume (percent: %d, value: %.2f)\n", local_volume, airplay_volume);
      prepared->volume = local_volume;
    }
  else
    {
      DPRINTF(E_LOG, L_PLAYER, "Shairport airplay volume out of range (-144.0, [-30.0 - 0.0]): %.2f\n", airplay_volume);
      goto error;
    }

  return 0;

 error:
  return -1;
}

static int
parse_picture(struct pipe_metadata_prepared *prepared, uint8_t *data, int data_len)
{
  struct input_metadata *m = &prepared->input_metadata;
  const char *ext;
  ssize_t ret;

  free(m->artwork_url);
  m->artwork_url = NULL;

  if (data_len < 2 || data_len > PIPE_PICTURE_SIZE_MAX)
    {
      DPRINTF(E_WARN, L_PLAYER, "Unsupported picture size (%d) from Shairport metadata pipe\n", data_len);
      goto error;
    }

  if (data[0] == 0xff && data[1] == 0xd8)
    ext = ".jpg";
  else if (data[0] == 0x89 && data[1] == 0x50)
    ext = ".png";
  else
    {
      DPRINTF(E_LOG, L_PLAYER, "Unsupported picture format from Shairport metadata pipe\n");
      goto error;
    }

  prepared->pict_tmpfile_fd = pict_tmpfile_recreate(prepared, ext);
  if (prepared->pict_tmpfile_fd < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not open tmpfile for pipe artwork '%s': %s\n", prepared->pict_tmpfile_path, strerror(errno));
      goto error;
    }

  ret = write(prepared->pict_tmpfile_fd, data, data_len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Error writing artwork from metadata pipe to '%s': %s\n", prepared->pict_tmpfile_path, strerror(errno));
      goto error;
    }
  else if (ret != data_len)
    {
      DPRINTF(E_LOG, L_PLAYER, "Incomplete write of artwork to '%s' (%zd/%d)\n", prepared->pict_tmpfile_path, ret, data_len);
      goto error;
    }

  DPRINTF(E_DBG, L_PLAYER, "Wrote pipe artwork to '%s'\n", prepared->pict_tmpfile_path);

  m->artwork_url = safe_asprintf("file:%s", prepared->pict_tmpfile_path);

  return 9;

 error:
  return -1;
}

static void
log_incoming(int severity, const char *msg, uint32_t type, uint32_t code, int data_len)
{
  char typestr[5];
  char codestr[5];

  dmap_val2str(typestr, type);
  dmap_val2str(codestr, code);

  DPRINTF(severity, L_PLAYER, "%s (type=%s, code=%s, len=%d)\n", msg, typestr, codestr, data_len);
}

// Trims leading/trailing whitespace from a NUL-terminated string, shifting
// the remaining content down to the start of the buffer (rather than just
// advancing the returned pointer past leading whitespace) so that the
// original allocation base stays valid to free(). Mirrors the effect of the
// trim() helper that used to live in the now-removed misc_xml.c, but must
// preserve the malloc base since xml_tag_dup()'s caller frees what this
// returns.
static char *
xml_trim(char *str)
{
  char *start;
  size_t len;

  if (!str)
    return NULL;

  start = str;
  while (isspace((unsigned char)*start))
    start++;

  len = strlen(start);
  while (len > 0 && isspace((unsigned char)start[len - 1]))
    len--;

  if (start != str)
    memmove(str, start, len);
  str[len] = '\0';

  return str;
}

// Hand-rolled substitute for the libxml2-based xml_get_val() that used to
// extract "item/<tag>" values here (removed by 71272642 along with
// misc_xml.c). Finds the first <tag ...>...</tag> in "item" (tolerating
// attributes on the opening tag, e.g. <data encoding="base64">) and returns a
// newly allocated, trimmed, NUL-terminated copy of its content, or NULL if
// the tag is not present. Caller must free() the result.
static char *
xml_tag_dup(const char *item, const char *tag)
{
  char open_tag[32];
  char close_tag[32];
  const char *start;
  const char *val;
  const char *end;
  size_t len;
  char *result;

  snprintf(open_tag, sizeof(open_tag), "<%s", tag);
  snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

  start = strstr(item, open_tag);
  if (!start)
    return NULL;

  val = strchr(start, '>');
  if (!val)
    return NULL;
  val++;

  end = strstr(val, close_tag);
  if (!end)
    return NULL;

  len = end - val;
  result = malloc(len + 1);
  if (!result)
    return NULL;

  memcpy(result, val, len);
  result[len] = '\0';

  return xml_trim(result);
}

/* Example of pipe metadata item (Shairport-sync format):

<item><type>73736e63</type><code>6d647374</code><length>9</length>
<data encoding="base64">
NDE5OTg3OTU0</data></item>

The <length> field is informational only and is not consulted below; <data>
is optional (e.g. for "pend"/progress-only items) and, when present, is
base64-decoded with the existing b64_decode() helper (src/misc.c), same as
before the libxml2 removal.
*/
static int
parse_item_xml(uint32_t *type, uint32_t *code, uint8_t **data, int *data_len, const char *item)
{
  char *s;

  *type = 0;
  if ((s = xml_tag_dup(item, "type")))
    {
      sscanf(s, "%8x", type);
      free(s);
    }

  *code = 0;
  if ((s = xml_tag_dup(item, "code")))
    {
      sscanf(s, "%8x", code);
      free(s);
    }

  if (*type == 0 || *code == 0)
    {
      DPRINTF(E_DBG, L_PLAYER, "No type (%" PRIu32 ") or code (%" PRIu32 ") in pipe metadata item, skipping: %s\n", *type, *code, item);
      return -1;
    }

  *data = NULL;
  *data_len = 0;
  if ((s = xml_tag_dup(item, "data")))
    {
      *data = b64_decode(data_len, s);
      free(s);
      if (*data == NULL)
	{
	  DPRINTF(E_DBG, L_PLAYER, "Base64 decode of pipe metadata payload failed, skipping item: %s\n", item);
	  return -1;
	}
    }

  return 0;
}

static int
parse_item(enum pipe_metadata_msg *out_msg, struct pipe_metadata_prepared *prepared, const char *item)
{
  struct input_metadata *m = &prepared->input_metadata;
  uint32_t type;
  uint32_t code;
  uint8_t *data;
  int data_len;
  char **dstptr;
  enum pipe_metadata_msg message;
  int ret;

  ret = parse_item_xml(&type, &code, &data, &data_len, item);
  if (ret < 0)
    {
      // Malformed item (e.g. truncated read, unexpected format): skip it and
      // keep the metadata pipe open, don't tear down the watch over one bad
      // item.
      *out_msg = 0;
      return 0;
    }

  dstptr = NULL;
  message = PIPE_METADATA_MSG_METADATA;

  if (code == dmap_str2val("asal"))
    dstptr = &m->album;
  else if (code == dmap_str2val("asar"))
    dstptr = &m->artist;
  else if (code == dmap_str2val("minm"))
    dstptr = &m->title;
  else if (code == dmap_str2val("asgn"))
    dstptr = &m->genre;
  else if (code == dmap_str2val("prgr"))
    message = PIPE_METADATA_MSG_PROGRESS;
  else if (code == dmap_str2val("pvol"))
    message = PIPE_METADATA_MSG_VOLUME;
  else if (code == dmap_str2val("PICT"))
    message = PIPE_METADATA_MSG_PICTURE;
  else if (code == dmap_str2val("pfls"))
    message = PIPE_METADATA_MSG_FLUSH;
  else
    goto ignore;

  if (message != PIPE_METADATA_MSG_FLUSH && (!data || data_len == 0))
    {
      log_incoming(E_DBG, "Missing or pending Shairport metadata payload", type, code, data_len);
      goto ignore;
    }

  ret = 0;
  if (message == PIPE_METADATA_MSG_PROGRESS)
    ret = parse_progress(prepared, (char *)data);
  else if (message == PIPE_METADATA_MSG_VOLUME)
    ret = parse_volume(prepared, (char *)data);
  else if (message == PIPE_METADATA_MSG_PICTURE)
    ret= parse_picture(prepared, data, data_len);
  else if (dstptr)
    swap_pointers(dstptr, (char **)&data);

  if (ret < 0)
    goto ignore;

  log_incoming(E_DBG, "Applying Shairport metadata", type, code, data_len);

  *out_msg = message;
  free(data);
  return 0;

 ignore:
  *out_msg = 0;
  free(data);
  return 0;
}

static char *
extract_item(struct evbuffer *evbuf)
{
  struct evbuffer_ptr evptr;
  size_t size;
  char *item;

  evptr = evbuffer_search(evbuf, "</item>", strlen("</item>"), NULL);
  if (evptr.pos < 0)
    return NULL;

  size = evptr.pos + strlen("</item>") + 1;
  item = malloc(size);
  if (!item)
    return NULL;

  evbuffer_remove(evbuf, item, size - 1);
  item[size - 1] = '\0';

  return item;
}

// Parses the xml content of the evbuf into a parsed struct. The first arg is
// a bitmask describing all the item types that were found, e.g.
// PIPE_METADATA_MSG_VOLUME | PIPE_METADATA_MSG_METADATA. A malformed item is
// skipped (logged once by parse_item()/parse_item_xml()) rather than
// aborting the parse, so one bad item on the pipe never stops later items in
// the same buffer, or later reads, from being processed.
static int
pipe_metadata_parse(enum pipe_metadata_msg *out_msg, struct pipe_metadata_prepared *prepared, struct evbuffer *evbuf)
{
  enum pipe_metadata_msg message;
  char *item;
  int ret;

  *out_msg = 0;
  while ((item = extract_item(evbuf)))
    {
      ret = parse_item(&message, prepared, item);
      free(item);
      if (ret < 0)
	{
	  // Defensive: parse_item() currently always returns 0, skipping bad
	  // items internally. If that ever changes, don't let one bad item
	  // in the buffer block the rest of the batch.
	  DPRINTF(E_DBG, L_PLAYER, "Skipping unparseable item on metadata pipe\n");
	  continue;
	}

      *out_msg |= message;
    }

  return 0;
}


/* ------------------------------ PIPE WATCHING ----------------------------- */
/*                                 Thread: pipe                               */

// Some data arrived on a pipe we watch - let's autostart playback
static void
pipe_read_cb(evutil_socket_t fd, short event, void *arg)
{
  struct pipe *pipe = arg;
  struct player_status status;
  int ret;

  // The watch event is one-shot (EV_READ without EV_PERSIST), so every exit
  // from this callback that does not hand the pipe over to the input must
  // re-arm the watch -- otherwise autostart is dead until something else
  // resets it, and the writer's data just backs up in the FIFO.
  ret = player_get_status(&status);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Pipe autostart of '%s' failed because state of player is unknown\n", pipe->path);
      watch_reset(pipe);
      return;
    }

  // Only an actually PLAYING player owns the pipe. A player merely parked on
  // the pipe item -- paused, or stopped without clearing the item -- is not
  // draining it, and swallowing the event here would leave the FIFO backing
  // up with the watch disarmed. Fall through and (re)start playback instead.
  if (status.id == pipe->id && status.status == PLAY_PLAYING)
    {
      DPRINTF(E_INFO, L_PLAYER, "Pipe '%s' already playing\n", pipe->path);
      return; // We are already playing the pipe
    }

  DPRINTF(E_INFO, L_PLAYER, "Autostarting pipe '%s' (fd %d)\n", pipe->path, fd);

  player_playback_stop();

  ret = player_playback_start_byid(pipe->id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Autostarting pipe '%s' (fd %d) failed\n", pipe->path, fd);
      watch_reset(pipe);
      return;
    }

  pipe_autostart_id = pipe->id;
}

static enum command_state
pipe_watch_reset(void *arg, int *retval)
{
  union pipe_arg *cmdarg = arg;
  struct pipe *pipe;

  pipe_autostart_id = 0;

  if (pipe_quiesced)
    return COMMAND_END;

  pipe = pipelist_find(pipe_watch_list, cmdarg->id);
  if (!pipe)
    return COMMAND_END;

  *retval = watch_reset(pipe);

  return COMMAND_END;
}

static enum command_state
pipe_watch_update(void *arg, int *retval)
{
  union pipe_arg *cmdarg = arg;
  struct pipe *pipelist;
  struct pipe *pipe;
  struct pipe *next;
  struct pipe *wanted;
  int count;

  if (cmdarg)
    pipelist = cmdarg->pipelist;
  else
    pipelist = NULL;

  // Removes pipes that are gone from the watchlist
  for (pipe = pipe_watch_list; pipe; pipe = next)
    {
      next = pipe->next;

      wanted = pipelist_find(pipelist, pipe->id);

      if (!wanted)
	{
	  DPRINTF(E_DBG, L_PLAYER, "Pipe watch deleted: '%s'\n", pipe->path);
	  watch_del(pipe);
	  pipelist_remove(&pipe_watch_list, pipe); // Will free pipe
	}
      else if (strcmp(pipe->path, wanted->path) != 0)
	{
	  DPRINTF(E_DBG, L_PLAYER, "Pipe watch path changed: '%s' -> '%s'\n", pipe->path, wanted->path);
	  watch_del(pipe);
	  pipelist_remove(&pipe_watch_list, pipe); // Will free pipe
	}
    }

  // Looks for new pipes and adds them to the watchlist
  for (pipe = pipelist, count = 0; pipe; pipe = next, count++)
    {
      next = pipe->next;

      if (count > PIPE_MAX_WATCH)
	{
	  DPRINTF(E_LOG, L_PLAYER, "Max open pipes reached (%d), will not watch '%s'\n", PIPE_MAX_WATCH, pipe->path);
	  pipe_free(pipe);
	  continue;
	}

      if (!pipelist_find(pipe_watch_list, pipe->id))
	{
	  DPRINTF(E_DBG, L_PLAYER, "Pipe watch added: '%s'\n", pipe->path);
	  watch_add(pipe);
	  pipelist_add(&pipe_watch_list, pipe); // Changes pipe->next
	}
      else
	{
	  DPRINTF(E_DBG, L_PLAYER, "Pipe watch exists: '%s'\n", pipe->path);
	  pipe_free(pipe);
	}
    }

  *retval = 0;
  return COMMAND_END;
}

// Reattaches any watch that is not (or no longer) reading the fifo its path
// points at, so that autostart keeps working across a fifo being recreated or
// showing up later than we did
static void
pipe_watch_health_cb(evutil_socket_t fd, short event, void *arg)
{
  struct timeval tv = { PIPE_WATCH_HEALTH_INTERVAL, 0 };
  struct pipe *pipe;
  int ret;

  if (pipe_quiesced)
    return;

  for (pipe = pipe_watch_list; pipe; pipe = pipe->next)
    {
      // The pipe we autostarted is reset by pipe_watch_reset when playback stops
      if (pipe->id == pipe_autostart_id)
	continue;

      if (!watch_is_stale(pipe))
	continue;

      ret = watch_reset(pipe);
      if (ret < 0)
	{
	  pipe->watch_failed = true; // Silences repeat attempts until we succeed
	  continue;
	}

      DPRINTF(E_LOG, L_PLAYER, "Pipe watch (re)attached: '%s'\n", pipe->path);
      pipe->watch_failed = false;
    }

  evtimer_add(pipe_watch_health_ev, &tv);
}

// Runs on the pipe thread. Disarms every watch and the health timer and
// latches pipe_quiesced, so that from this point nothing on the pipe thread
// can start playback or re-arm a watch. State (the watch list, the command
// base) stays alive for the normal deinit to tear down later.
static enum command_state
pipe_quiesce_cmd(void *arg, int *retval)
{
  struct pipe *pipe;

  pipe_quiesced = true;

  if (pipe_watch_health_ev)
    event_del(pipe_watch_health_ev);

  for (pipe = pipe_watch_list; pipe; pipe = pipe->next)
    watch_del(pipe);

  *retval = 0;
  return COMMAND_END;
}

// Called from the shutdown path before any thread or command base is torn
// down: once this returns, the pipe input generates no new work -- no
// autostarts, no watch re-arms -- which the teardown ordering depends on.
static void
quiesce(void)
{
  if (!tid_pipe)
    return;

  commands_exec_sync(cmdbase, pipe_quiesce_cmd, NULL, NULL);
}

static void *
pipe_thread_run(void *arg)
{
  thread_setname("pipe");

  event_base_dispatch(evbase_pipe);

  pthread_exit(NULL);
}


/* --------------------------- METADATA PIPE HANDLING ----------------------- */
/*                                Thread: worker                              */

// Caller must hold pipe_metadata.lock
static void
pipe_metadata_watch_del_locked(void)
{
  if (!pipe_metadata.pipe)
    return;

  evbuffer_free(pipe_metadata.evbuf);
  watch_del(pipe_metadata.pipe);
  pipe_free(pipe_metadata.pipe);
  pipe_metadata.pipe = NULL;

  pict_tmpfiles_remove(&pipe_metadata.prepared);
}

static void
pipe_metadata_watch_del(void *arg)
{
  pthread_mutex_lock(&pipe_metadata.lock);
  pipe_metadata_watch_del_locked();
  pthread_mutex_unlock(&pipe_metadata.lock);
}

// Some metadata arrived on a pipe we watch
static void
pipe_metadata_read_cb(evutil_socket_t fd, short event, void *arg)
{
  enum pipe_metadata_msg message;
  size_t len;
  int ret;

  // .pipe and .evbuf are shared with the worker pool (add/del) and the input
  // thread (stop), so use mutex. Lock ordering: pipe_metadata.lock (outer),
  // then pipe_metadata.prepared.lock (inner) - never the reverse.
  pthread_mutex_lock(&pipe_metadata.lock);

  // The watch may have been torn down (and .pipe/.evbuf freed) between this
  // event firing and the lock being acquired
  if (!pipe_metadata.pipe || !pipe_metadata.evbuf)
    {
      pthread_mutex_unlock(&pipe_metadata.lock);
      return;
    }

  ret = evbuffer_read(pipe_metadata.evbuf, pipe_metadata.pipe->fd, PIPE_READ_MAX);
  if (ret < 0)
    {
      if (errno != EAGAIN)
	pipe_metadata_watch_del_locked();
      pthread_mutex_unlock(&pipe_metadata.lock);
      return;
    }
  else if (ret == 0)
    {
      // Reset the pipe
      ret = watch_reset(pipe_metadata.pipe);
      if (ret < 0)
	{
	  pthread_mutex_unlock(&pipe_metadata.lock);
	  return;
	}
      goto readd;
    }

  len = evbuffer_get_length(pipe_metadata.evbuf);
  if (len > PIPE_METADATA_BUFLEN_MAX)
    {
      DPRINTF(E_LOG, L_PLAYER, "Buffer for metadata pipe '%s' is full, discarding %zu bytes\n", pipe_metadata.pipe->path, len);
      evbuffer_drain(pipe_metadata.evbuf, len);
      goto readd;
    }

  // .parsed is shared with the input thread (see metadata_get), so use mutex.
  // Note that this means _parse() must not do anything that could cause a
  // deadlock (e.g. make a sync call to the player thread).
  pthread_mutex_lock(&pipe_metadata.prepared.lock);
  ret = pipe_metadata_parse(&message, &pipe_metadata.prepared, pipe_metadata.evbuf);
  pthread_mutex_unlock(&pipe_metadata.prepared.lock);
  if (ret < 0)
    {
      // pipe_metadata_parse() skips malformed items internally and should
      // never actually return an error, but if it ever does, don't tear
      // down the watch over it - just drop this batch and keep reading.
      DPRINTF(E_WARN, L_PLAYER, "Error parsing incoming data on metadata pipe '%s', dropping batch\n", pipe_metadata.pipe->path);
      goto readd;
    }

  if (message & (PIPE_METADATA_MSG_METADATA | PIPE_METADATA_MSG_PROGRESS | PIPE_METADATA_MSG_PICTURE))
    pipe_metadata.is_new = 1; // Trigger notification to player in playback loop
  if (message & PIPE_METADATA_MSG_VOLUME)
    player_volume_set(pipe_metadata.prepared.volume);
  if (message & PIPE_METADATA_MSG_FLUSH)
    player_playback_flush();

 readd:
  if (pipe_metadata.pipe && pipe_metadata.pipe->ev)
    event_add(pipe_metadata.pipe->ev, NULL);

  pthread_mutex_unlock(&pipe_metadata.lock);
}

// Caller must hold pipe_metadata.lock
static void
pipe_metadata_watch_add_locked(const char *base_path)
{
  char path[PATH_MAX];
  int ret;

  ret = snprintf(path, sizeof(path), "%s.metadata", base_path);
  if ((ret < 0) || (ret > sizeof(path)))
    return;

  pipe_metadata_watch_del_locked(); // Just in case we somehow already have a metadata pipe open

  pipe_metadata.pipe = pipe_create(path, 0, PIPE_METADATA, pipe_metadata_read_cb);
  pipe_metadata.evbuf = evbuffer_new();

  ret = watch_add(pipe_metadata.pipe);
  if (ret < 0)
    {
      evbuffer_free(pipe_metadata.evbuf);
      pipe_free(pipe_metadata.pipe);
      pipe_metadata.pipe = NULL;
      return;
    }
}

static void
pipe_metadata_watch_add(void *arg)
{
  char *base_path = arg;

  pthread_mutex_lock(&pipe_metadata.lock);
  pipe_metadata_watch_add_locked(base_path);
  pthread_mutex_unlock(&pipe_metadata.lock);
}


/* ----------------------- PIPE WATCH THREAD START/STOP --------------------- */
/*                             Thread: filescanner                            */

static void
pipe_thread_start(void)
{
  struct timeval tv = { PIPE_WATCH_HEALTH_INTERVAL, 0 };

  CHECK_NULL(L_PLAYER, evbase_pipe = event_base_new());
  CHECK_NULL(L_PLAYER, cmdbase = commands_base_new(evbase_pipe, NULL));
  CHECK_NULL(L_PLAYER, pipe_watch_health_ev = evtimer_new(evbase_pipe, pipe_watch_health_cb, NULL));
  evtimer_add(pipe_watch_health_ev, &tv);
  CHECK_ERR(L_PLAYER, pthread_create(&tid_pipe, NULL, pipe_thread_run, NULL));
}

static void
pipe_thread_stop(void)
{
  int ret;

  if (!tid_pipe)
    return;

  commands_exec_sync(cmdbase, pipe_watch_update, NULL, NULL);
  commands_base_destroy(cmdbase);

  ret = pthread_join(tid_pipe, NULL);
  if (ret != 0)
    DPRINTF(E_LOG, L_PLAYER, "Could not join pipe thread: %s\n", strerror(errno));

  event_free(pipe_watch_health_ev);
  pipe_watch_health_ev = NULL;

  event_base_free(evbase_pipe);
  tid_pipe = 0;
}

// Makes a pipelist from the configured pipe_path. Uses a fixed synthetic id=1.
// Returns NULL if no pipe_path is configured.
static struct pipe *
pipelist_create_path(const char *path)
{
  struct pipe *head;
  struct pipe *pipe;

  if (!path)
    return NULL;

  pipe = pipe_create(path, 1, PIPE_PCM, pipe_read_cb);

  head = NULL;
  pipelist_add(&head, pipe);

  return head;
}

static struct pipe *
pipelist_create(void)
{
  return pipelist_create_path(pipe_path);
}

static int
pipe_path_update(const char *path)
{
  char *new_path;

  new_path = path ? strdup(path) : NULL;
  if (path && !new_path)
    return -1;

  free(pipe_path);
  pipe_path = new_path;

  return 0;
}

int
pipe_path_validate(const char *path)
{
  int fd;

  if (!path)
    return 0;

  fd = pipe_open(path, true);
  if (fd < 0)
    return -1;

  pipe_close(fd);
  return 0;
}

static int
pipe_runtime_watch_update(const char *path, int autostart)
{
  union pipe_arg cmdarg;
  struct pipe *new_pipelist;
  bool watch_before;
  bool watch_after;
  int ret;

  watch_before = (pipe_autostart && pipe_path);
  watch_after = (autostart && path);

  if (!watch_before && !watch_after)
    return 0;

  new_pipelist = watch_after ? pipelist_create_path(path) : NULL;
  if (watch_after && !new_pipelist)
    return -1;

  if (!watch_before && watch_after)
    pipe_thread_start();

  cmdarg.pipelist = new_pipelist;
  ret = commands_exec_sync(cmdbase, pipe_watch_update, NULL, &cmdarg);
  if (ret < 0)
    {
      if (new_pipelist)
	pipe_free(new_pipelist);

      if (!watch_before && watch_after)
	{
	  DPRINTF(E_LOG, L_PLAYER, "Pipe watch command failed after thread start; stopping pipe thread\n");
	  if (cmdbase)
	    pipe_thread_stop();
	}
      return -1;
    }

  if (watch_before && !watch_after)
    pipe_thread_stop();

  return 0;
}

static int
pipe_config_reload(void)
{
  const char *new_pipe_path;
  char *new_pipe_path_copy;
  int new_pipe_autostart;
  bool path_changed;
  bool autostart_changed;
  int ret;

  new_pipe_path = config_get_str("pipe_path", NULL);
  new_pipe_autostart = config_get_bool("pipe_autostart", true);

  path_changed = ((pipe_path == NULL && new_pipe_path != NULL)
               || (pipe_path != NULL && new_pipe_path == NULL)
               || (pipe_path && new_pipe_path && strcmp(pipe_path, new_pipe_path) != 0));
  autostart_changed = (pipe_autostart != new_pipe_autostart);
  if (!path_changed && !autostart_changed)
    return 0;

  ret = pipe_path_validate(new_pipe_path);
  if (ret < 0)
    return -1;

  new_pipe_path_copy = new_pipe_path ? strdup(new_pipe_path) : NULL;
  if (new_pipe_path && !new_pipe_path_copy)
    return -1;

  ret = pipe_runtime_watch_update(new_pipe_path, new_pipe_autostart);
  if (ret < 0)
    {
      free(new_pipe_path_copy);
      return -1;
    }

  free(pipe_path);
  pipe_path = new_pipe_path_copy;

  pipe_autostart = new_pipe_autostart;
  if (path_changed)
    pipe_autostart_id = 0;

  return path_changed ? 1 : 0;
}


/* --------------------------- PIPE INPUT INTERFACE ------------------------- */
/*                                Thread: input                               */

static int
setup(struct input_source *source)
{
  struct pipe *pipe;
  int fd;

  fd = pipe_open(source->path, 0);
  if (fd < 0)
    return -1;

  CHECK_NULL(L_PLAYER, source->evbuf = evbuffer_new());

  pipe = pipe_create(source->path, source->id, PIPE_PCM, NULL);

  pipe->fd = fd;
  pipe->is_autostarted = (source->id == pipe_autostart_id);

  worker_execute(pipe_metadata_watch_add, source->path, strlen(source->path) + 1, 0);

  source->input_ctx = pipe;

  source->quality.sample_rate = pipe_sample_rate;
  source->quality.bits_per_sample = pipe_bits_per_sample;
  source->quality.channels = 2;

  return 0;
}

static int
stop(struct input_source *source)
{
  struct pipe *pipe = source->input_ctx;
  union pipe_arg *cmdarg;

  DPRINTF(E_DBG, L_PLAYER, "Stopping pipe\n");

  if (source->evbuf)
    evbuffer_free(source->evbuf);

  pipe_close(pipe->fd);

  // Reset the pipe and start watching it again for new data. Must be async or
  // we will deadlock from the stop in pipe_read_cb().
  if (pipe_autostart && (cmdarg = malloc(sizeof(union pipe_arg))))
    {
      cmdarg->id = pipe->id;
      commands_exec_async(cmdbase, pipe_watch_reset, cmdarg);
    }

  pthread_mutex_lock(&pipe_metadata.lock);
  if (pipe_metadata.pipe)
    worker_execute(pipe_metadata_watch_del, NULL, 0, 0);
  pthread_mutex_unlock(&pipe_metadata.lock);

  pipe_free(pipe);

  source->input_ctx = NULL;
  source->evbuf = NULL;

  return 0;
}

static int
play(struct input_source *source)
{
  struct pipe *pipe = source->input_ctx;
  short flags;
  int ret;

  ret = evbuffer_read(source->evbuf, pipe->fd, PIPE_READ_MAX);
  if ((ret == 0) && (pipe->is_autostarted))
    {
      input_write(source->evbuf, NULL, INPUT_FLAG_EOF); // Autostop
      stop(source);
      return -1;
    }
  else if ((ret == 0) || ((ret < 0) && (errno == EAGAIN)))
    {
      input_wait();
      return 0; // Loop
    }
  else if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not read from pipe '%s': %s\n", source->path, strerror(errno));
      input_write(NULL, NULL, INPUT_FLAG_ERROR);
      stop(source);
      return -1;
    }

  flags = (pipe_metadata.is_new ? INPUT_FLAG_METADATA : 0);
  pipe_metadata.is_new = 0;

  input_write(source->evbuf, &source->quality, flags);

  // Periodic (30 s) pipe-input feed-rate diagnostics for the cumulative-dropout
  // hunt: the rate owntone pulls from the monitor's FIFO should track the
  // monitor's nominal output rate. A sustained shortfall here means the source
  // is starving playback (drift/underrun); a match rules the input side out.
  {
    static uint64_t       pipe_stats_bytes = 0;
    static struct timespec pipe_stats_last = { 0, 0 };
    struct timespec now_ts;

    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    if (pipe_stats_last.tv_sec == 0 && pipe_stats_last.tv_nsec == 0)
      pipe_stats_last = now_ts;
    pipe_stats_bytes += (ret > 0) ? (uint64_t)ret : 0;

    double dt = (now_ts.tv_sec - pipe_stats_last.tv_sec)
                + (now_ts.tv_nsec - pipe_stats_last.tv_nsec) / 1000000000.0;

    if (dt >= 30.0)
      {
        double bytes_per_s  = pipe_stats_bytes / dt;
        int    bpf          = 2 * (pipe_bits_per_sample / 8); // stereo
        double frames_per_s = (bpf > 0) ? bytes_per_s / bpf : 0.0;
        double dev          = frames_per_s - (double)pipe_sample_rate;

        if (dev < 0.0)
          dev = -dev;

        // The pipe feed should track the source's nominal rate. A short source
        // silence is still fed at nominal, so it does NOT trip this; a >0.5%
        // shortfall over 30 s means the source stopped feeding (e.g. the monitor
        // gated a long silence) and playback is starving - escalate to WARN.
        // The routine line stays at DEBUG.
        if (dev > 0.005 * (double)pipe_sample_rate)
          DPRINTF(E_WARN, L_PLAYER, "[stats] pipe input '%s': %.1f frames/s off nominal %d over %.0fs  <-- SOURCE FEED STARVING\n",
                  source->path, frames_per_s, pipe_sample_rate, dt);
        else
          DPRINTF(E_DBG, L_PLAYER, "[stats] pipe input '%s': %.0f bytes/s = %.1f frames/s (nominal %d) over %.0fs\n",
                  source->path, bytes_per_s, frames_per_s, pipe_sample_rate, dt);

        pipe_stats_bytes = 0;
        pipe_stats_last  = now_ts;
      }
  }

  return 0;
}

static int
metadata_get(struct input_metadata *metadata, struct input_source *source)
{
  pthread_mutex_lock(&pipe_metadata.prepared.lock);

  *metadata = pipe_metadata.prepared.input_metadata;

  // Ownership transferred to caller, null all pointers in the struct
  memset(&pipe_metadata.prepared.input_metadata, 0, sizeof(struct input_metadata));

  pthread_mutex_unlock(&pipe_metadata.prepared.lock);

  return 0;
}

// Thread: main
static int
init(void)
{
  const char *configured_pipe_path;
  int ret;

  CHECK_ERR(L_PLAYER, mutex_init(&pipe_metadata.prepared.lock));
  CHECK_ERR(L_PLAYER, mutex_init(&pipe_metadata.lock));

  pipe_metadata.prepared.pict_tmpfile_fd = -1;

  configured_pipe_path = config_get_str("pipe_path", NULL);
  ret = pipe_path_update(configured_pipe_path);
  if (ret < 0)
    return -1;

  pipe_autostart = config_get_bool("pipe_autostart", true);
  if (pipe_autostart && pipe_path)
    {
      union pipe_arg *cmdarg;

      cmdarg = malloc(sizeof(union pipe_arg));
      if (!cmdarg)
        return -1;

      cmdarg->pipelist = pipelist_create();
      if (!cmdarg->pipelist)
        {
          free(cmdarg);
        }
      else
        {
          pipe_thread_start();
          commands_exec_async(cmdbase, pipe_watch_update, cmdarg);
        }
    }

  pipe_sample_rate = config_get_int("pipe_sample_rate", 44100);
  if (pipe_sample_rate != 44100 && pipe_sample_rate != 48000 && pipe_sample_rate != 88200 && pipe_sample_rate != 96000)
    {
      DPRINTF(E_FATAL, L_PLAYER, "The configuration of pipe_sample_rate is invalid: %d\n", pipe_sample_rate);
      return -1;
    }

  pipe_bits_per_sample = config_get_int("pipe_bits_per_sample", 16);
  if (pipe_bits_per_sample != 16 && pipe_bits_per_sample != 32)
    {
      DPRINTF(E_FATAL, L_PLAYER, "The configuration of pipe_bits_per_sample is invalid: %d\n", pipe_bits_per_sample);
      return -1;
    }

  return 0;
}

static void
deinit(void)
{
  if (pipe_autostart && pipe_path)
    pipe_thread_stop();

  free(pipe_path);
  pipe_path = NULL;
  pipe_autostart_id = 0;

  CHECK_ERR(L_PLAYER, pthread_mutex_destroy(&pipe_metadata.prepared.lock));
  CHECK_ERR(L_PLAYER, pthread_mutex_destroy(&pipe_metadata.lock));
}

struct input_definition input_pipe =
{
  .name = "pipe",
  .type = INPUT_TYPE_PIPE,
  .disabled = 0,
  .setup = setup,
  .play = play,
  .stop = stop,
  .metadata_get = metadata_get,
  .init = init,
  .config_reload = pipe_config_reload,
  .quiesce = quiesce,
  .deinit = deinit,
};
