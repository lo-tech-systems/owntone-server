/*
 * artwork.h — compatibility shim for owntone-minimal
 *
 * The upstream airplay.c and raop.c call artwork_get_by_queue_item_id().
 * In the full OwnTone build this function queries the SQLite artwork cache;
 * in owntone-minimal we read artwork directly from the file referenced by
 * queue_item->artwork_url (must be a "file:" URI).
 *
 * Copyright (C) 2025 OwnTone-Minimal contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __ARTWORK_H__
#define __ARTWORK_H__

#include <event2/buffer.h>

/* Artwork format constants used by the upstream files */
#define ART_FMT_PNG   1
#define ART_FMT_JPEG  2
#define ART_FMT_VP8   3

/* Default dimensions requested by the upstream files */
#define ART_DEFAULT_HEIGHT 600
#define ART_DEFAULT_WIDTH  600

/*
 * Fetch artwork for the queue item identified by item_id and append the raw
 * image bytes to evbuf.
 *
 * Parameters:
 *   evbuf    – destination buffer (caller-owned, must not be NULL)
 *   item_id  – queue item id (uint32_t cast to int)
 *   max_w    – ignored in this implementation
 *   max_h    – ignored in this implementation
 *   format   – ignored in this implementation (format is inferred from file ext)
 *
 * Returns the ART_FMT_* constant for the loaded image on success,
 * or -1 if no artwork is available or an error occurred.
 */
int artwork_get_by_queue_item_id(struct evbuffer *evbuf, int item_id,
                                 int max_w, int max_h, int format);

#endif /* !__ARTWORK_H__ */
