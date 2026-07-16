#ifndef __AIRPLAY_BUFFERED_H__
#define __AIRPLAY_BUFFERED_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct event_base;

struct airplay_buffered_stream;

// Connects (nonblocking) to address:data_port and sets up the ChaCha20-Poly1305
// cipher for the type-103 buffered-audio TCP transport. On success *bs is set
// and owns the socket/evbuffer/cipher. Returns 0 on success, -1 on error.
int
airplay_buffered_start(struct airplay_buffered_stream **bs, struct event_base *evbase, const char *address, unsigned short data_port, const uint8_t *shk, size_t shk_len, uint32_t format_id);

// Frames, encrypts and queues one AAC frame for asynchronous transmission.
// Never blocks the calling thread. Returns 0 on success, -1 if the stream
// has failed (caller should treat this as a session failure).
int
airplay_buffered_write(struct airplay_buffered_stream *bs, const uint8_t *frame, size_t len, uint32_t seqnum, uint32_t rtptime, bool marker);

// Returns the number of bytes currently queued for transmission (i.e. not
// yet written to the socket).
size_t
airplay_buffered_pending(struct airplay_buffered_stream *bs);

// Closes the socket and frees all resources owned by bs.
void
airplay_buffered_stop(struct airplay_buffered_stream *bs);

#endif  /* !__AIRPLAY_BUFFERED_H__ */
