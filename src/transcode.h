
#ifndef __TRANSCODE_H__
#define __TRANSCODE_H__

#include <event2/buffer.h>

#include "misc.h"

enum transcode_profile
{
  // Decodes/resamples the best audio stream into PCM16 (no wav headers)
  XCODE_PCM16,
  // Decodes/resamples the best audio stream into PCM32 (no wav headers).
  // Used ahead of a 24-bit encoder so a >16-bit source isn't truncated to
  // 16 bits before encoding.
  XCODE_PCM32,
  // Transcodes the best audio stream to raw ALAC (no container)
  XCODE_ALAC,
  // Transcodes/upmixes the best audio stream to raw AAC-LC 48kHz 5.1 (no
  // container), using ffmpeg's "surround" filter (frequency-domain steering)
  // to derive the surround channels from a stereo source
  XCODE_AAC48K_51_DECODE,
  // Transcodes the best audio stream to raw AAC-LC 48kHz 5.1 (no container),
  // with a static pan matrix that places the stereo source in the front
  // left/right and LFE only - centre and rear channels are silent. This is
  // the "stereo, delivered as a 5.1 stream" payload for AirPlay 2 receivers
  // that only accept a 5.1 bufferStream format but are being fed plain
  // stereo content.
  XCODE_AAC48K_51_STEREO,
  // Raw AAC-LC 48kHz stereo (no container) - the buffered AirPlay 2 stereo
  // payload for bufferStream format 23
  XCODE_AAC48K_STEREO,
  // Raw AAC-LC 44.1kHz stereo (no container) - buffered AirPlay 2 stereo
  // payload for bufferStream format 22. The pipe is nominally 48kHz; this
  // is the resampling fallback path for 44.1kHz-only receivers, not the
  // no-resample case.
  XCODE_AAC44K_STEREO,
  // Raw ALAC 48kHz/24-bit stereo (no container) - the buffered AirPlay 2
  // stereo payload for bufferStream format 21
  XCODE_ALAC48K_24_STEREO,
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

/* Pops the byte size of the next individual encoder packet contained in the
 * output most recently produced by transcode_encode(). One transcode_encode()
 * call can write several encoder packets back to back (e.g. two 352-sample
 * ALAC frames), and some consumers (AirPlay buffered audio) must frame each
 * one separately. Only meaningful for raw ("data") output profiles, where the
 * muxer writes packet payloads verbatim so the sizes add up to the bytes
 * returned by transcode_encode().
 *
 * @in  ctx        Encode context
 * @return         Size in bytes of the next packet, 0 if none tracked
 */
int
transcode_encode_packet_size_next(struct encode_ctx *ctx);

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
