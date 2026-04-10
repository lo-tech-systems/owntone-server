/*
 * artwork.c — compatibility shim for owntone-minimal
 *
 * Implements artwork_get_by_queue_item_id() without the SQLite artwork cache.
 * Artwork is loaded directly from a local file referenced by
 * queue_item->artwork_url (must be a "file:" URI).
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
#include <strings.h>  /* strcasecmp */
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <event2/buffer.h>

#include "artwork.h"
#include "db.h"     /* db_queue_fetch_byitemid, free_queue_item */
#include "logger.h"

int
artwork_get_by_queue_item_id(struct evbuffer *evbuf, int item_id,
                             int max_w, int max_h, int format)
{
  struct db_queue_item *qi;
  const char *url;
  const char *path;
  const char *ext;
  int fmt;
  int fd;
  int ret;

  (void)max_w;
  (void)max_h;
  (void)format;

  qi = db_queue_fetch_byitemid((uint32_t)item_id);
  if (!qi)
    {
      DPRINTF(E_DBG, L_MISC, "artwork: queue item %d not found\n", item_id);
      return -1;
    }

  url = qi->artwork_url;
  if (!url || strncmp(url, "file:", 5) != 0)
    {
      free_queue_item(qi, 0);
      return -1;
    }

  path = url + 5; /* skip "file:" prefix */

  ext = strrchr(path, '.');
  if (!ext)
    {
      free_queue_item(qi, 0);
      return -1;
    }

  if (strcasecmp(ext, ".png") == 0)
    fmt = ART_FMT_PNG;
  else if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0)
    fmt = ART_FMT_JPEG;
  else
    {
      DPRINTF(E_DBG, L_MISC, "artwork: unsupported extension '%s'\n", ext);
      free_queue_item(qi, 0);
      return -1;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      DPRINTF(E_WARN, L_MISC, "artwork: could not open '%s': %s\n",
              path, strerror(errno));
      free_queue_item(qi, 0);
      return -1;
    }

  /* Read the entire file into the evbuffer in chunks */
  do
    {
      ret = evbuffer_read(evbuf, fd, 65536);
    }
  while (ret > 0);

  if (ret < 0)
    {
      DPRINTF(E_WARN, L_MISC, "artwork: read error on '%s': %s\n",
              path, strerror(errno));
      close(fd);
      free_queue_item(qi, 0);
      return -1;
    }

  close(fd);
  free_queue_item(qi, 0);
  return fmt;
}
