/*
 * dmap_common.c — compatibility shim for owntone-minimal
 *
 * Implements dmap_encode_queue_metadata() using inline DMAP encoding.
 * Produces a single "mlit" container holding minm (title), asar (artist)
 * and asal (album) text fields.
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

#include <stdint.h>
#include <string.h>

#include <event2/buffer.h>

#include "misc.h"          /* htobe32 */
#include "dmap_common.h"
#include "db.h"            /* struct db_queue_item */
#include "logger.h"

/* Write a single DMAP text field (4-byte tag + 4-byte length + data) */
#define DMAP_FIELD(buf, t, s)                                                 \
  do {                                                                        \
    uint32_t _t = htobe32(((uint32_t)(t)[0] << 24) |                         \
                          ((uint32_t)(t)[1] << 16) |                          \
                          ((uint32_t)(t)[2] <<  8) |                          \
                           (uint32_t)(t)[3]);                                 \
    uint32_t _l = htobe32((uint32_t)strlen(s));                               \
    evbuffer_add((buf), &_t, 4);                                              \
    evbuffer_add((buf), &_l, 4);                                              \
    evbuffer_add((buf), (s), strlen(s));                                      \
  } while (0)

int
dmap_encode_queue_metadata(struct evbuffer *evbuf, struct evbuffer *tmp,
                           struct db_queue_item *queue_item)
{
  struct evbuffer *inner;
  uint32_t tag;
  uint32_t len;
  const char *title;
  const char *artist;
  const char *album;
  int ret = 0;

  (void)tmp; /* unused in minimal implementation */

  if (!evbuf || !queue_item)
    return -1;

  title  = queue_item->title  ? queue_item->title  : "";
  artist = queue_item->artist ? queue_item->artist : "";
  album  = queue_item->album  ? queue_item->album  : "";

  inner = evbuffer_new();
  if (!inner)
    {
      DPRINTF(E_LOG, L_MISC, "dmap: could not allocate inner buffer\n");
      return -1;
    }

  DMAP_FIELD(inner, "minm", title);
  DMAP_FIELD(inner, "asar", artist);
  DMAP_FIELD(inner, "asal", album);

  tag = htobe32(((uint32_t)'m' << 24) | ((uint32_t)'l' << 16) |
                ((uint32_t)'i' <<  8) |  (uint32_t)'t');
  len = htobe32((uint32_t)evbuffer_get_length(inner));

  if (evbuffer_add(evbuf, &tag, 4) < 0 ||
      evbuffer_add(evbuf, &len, 4) < 0 ||
      evbuffer_add_buffer(evbuf, inner) < 0)
    {
      DPRINTF(E_LOG, L_MISC, "dmap: evbuffer write error\n");
      ret = -1;
    }

  evbuffer_free(inner);
  return ret;
}

#undef DMAP_FIELD
