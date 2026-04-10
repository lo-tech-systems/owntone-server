
#ifndef __TRANSCODE_H__
#define __TRANSCODE_H__

#include <event2/buffer.h>

#include "misc.h"

enum transcode_profile
{
  // Decodes/resamples the best audio stream into PCM16 (no wav headers)
  XCODE_PCM16,
  // Transcodes the best audio stream to raw ALAC (no container)
  XCODE_ALAC,
};

enum transcode_seek_type
{
  XCODE_SEEK_SIZE,
  XCODE_SEEK_SET,
  XCODE_SEEK_CUR,
};

typedef void transcode_frame;
typedef int64_t(*transcode_seekfn)(void *arg, int64_t offset, enum transcode_seek_type seek_type);


struct decode_ctx;
struct encode_ctx;

struct transcode_evbuf_io
{
  struct evbuffer *evbuf;

  // Set to null if no seek support required
  transcode_seekfn seekfn;
  void *seekfn_arg;
};

struct transcode_encode_setup_args
{
  enum transcode_profile profile;
  struct media_quality *quality;
  struct decode_ctx *src_ctx;
  struct transcode_evbuf_io *evbuf_io;
  struct evbuffer *prepared_header;
  int width;
  int height;
};


// Setting up
struct decode_ctx *
transcode_decode_setup_raw(enum transcode_profile profile, struct media_quality *quality);

struct encode_ctx *
transcode_encode_setup(struct transcode_encode_setup_args args);

// Cleaning up
void
transcode_decode_cleanup(struct decode_ctx **ctx);

void
transcode_encode_cleanup(struct encode_ctx **ctx);

// Encoding

/* Encodes and remuxes a frame. Also resamples if needed.
 *
 * @out evbuf      An evbuffer filled with remuxed data
 * @in  ctx        Encode context
 * @in  frame      The decoded frame to encode, e.g. from transcode_frame_new
 * @in  eof        If true the muxer will write a trailer to the output
 * @return         Bytes added if OK, negative if error
 */
int
transcode_encode(struct evbuffer *evbuf, struct encode_ctx *ctx, transcode_frame *frame, int eof);

/* Converts a buffer with raw data to a frame that can be passed directly to the
 * transcode_encode() function. It does not copy, so if you free the data the
 * frame will become invalid.
 *
 * @in  data       Buffer with raw data
 * @in  size       Size of buffer
 * @in  nsamples   Number of samples in the buffer
 * @in  quality    Sample rate, bits per sample and channels
 * @return         Opaque pointer to frame if OK, otherwise NULL
 */
transcode_frame *
transcode_frame_new(void *data, size_t size, int nsamples, struct media_quality *quality);
void
transcode_frame_free(transcode_frame *frame);

#endif /* !__TRANSCODE_H__ */
