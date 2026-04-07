
#ifndef __CACHE_H__
#define __CACHE_H__

#include <event2/buffer.h>

/* --------------------------- Transcode cache API  ------------------------- */

int
cache_xcode_header_get(struct evbuffer *evbuf, int *cached, uint32_t id, const char *format);

int
cache_xcode_toggle(bool enable);



/* ------------------------------- Cache API  ------------------------------- */

int
cache_init(void);

void
cache_deinit(void);

#endif /* !__CACHE_H__ */
