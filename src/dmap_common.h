/*
 * dmap_common.h — compatibility shim for owntone-minimal
 *
 * The upstream airplay.c and raop.c call dmap_encode_queue_metadata().
 * In the full OwnTone build this function produces rich DMAP metadata
 * including ratings, genre, etc.  In owntone-minimal we encode only the
 * three text fields used by the AirPlay / RAOP path (title, artist, album).
 *
 * Copyright (C) 2025 OwnTone-Minimal contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __DMAP_COMMON_H__
#define __DMAP_COMMON_H__

#include <event2/buffer.h>

#include "db.h"   /* struct db_queue_item */

/*
 * Encode title, artist and album from queue_item as a DMAP "mlit" container
 * and append it to evbuf.
 *
 * The tmp buffer is accepted for API compatibility with the upstream signature
 * but is not used in this implementation.
 *
 * Returns 0 on success, -1 on error.
 */
int dmap_encode_queue_metadata(struct evbuffer *evbuf, struct evbuffer *tmp,
                               struct db_queue_item *queue_item);

#endif /* !__DMAP_COMMON_H__ */
