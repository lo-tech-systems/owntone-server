/*
 * Copyright (C) 2015-17 Espen Jurgensen
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
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>

#include "logger.h"
#include "misc.h"
#include "transcode.h"

// Switches for compability with ffmpeg's ever changing API
#define USE_CONST_AVFORMAT (LIBAVFORMAT_VERSION_MAJOR > 59) || ((LIBAVFORMAT_VERSION_MAJOR == 59) && (LIBAVFORMAT_VERSION_MINOR > 15))
#define USE_CONST_AVCODEC (LIBAVFORMAT_VERSION_MAJOR > 59) || ((LIBAVFORMAT_VERSION_MAJOR == 59) && (LIBAVFORMAT_VERSION_MINOR > 15))
#define USE_NO_CLEAR_AVFMT_NOFILE (LIBAVFORMAT_VERSION_MAJOR > 59) || ((LIBAVFORMAT_VERSION_MAJOR == 59) && (LIBAVFORMAT_VERSION_MINOR > 15))
#define USE_CH_LAYOUT (LIBAVCODEC_VERSION_MAJOR > 59) || ((LIBAVCODEC_VERSION_MAJOR == 59) && (LIBAVCODEC_VERSION_MINOR > 24))
#define USE_CONST_AVIO_WRITE_PACKET (LIBAVFORMAT_VERSION_MAJOR > 61) || ((LIBAVFORMAT_VERSION_MAJOR == 61) && (LIBAVFORMAT_VERSION_MINOR > 0))
#define USE_AVCODEC_GET_SUPPORTED_CONFIG (LIBAVCODEC_VERSION_MAJOR > 61) || ((LIBAVCODEC_VERSION_MAJOR == 61) && (LIBAVCODEC_VERSION_MINOR > 13))

// Buffer size for reading/writing output evbuffers
#define AVIO_BUFFER_SIZE 4096
// Size of the wav header
#define WAV_HEADER_LEN 44
// Max filters in a filtergraph
#define MAX_FILTERS 9

// Used for passing errors to DPRINTF (can't count on av_err2str being present)
static char errbuf[64];

// The settings struct will be filled out based on the profile enum
struct settings_ctx
{
  bool encode_video;
  bool encode_audio;

  // Output format (for the muxer)
  const char *format;

  // Audio settings
  enum AVCodecID audio_codec;
  int sample_rate;
#if USE_CH_LAYOUT
  AVChannelLayout channel_layout;
#else
  uint64_t channel_layout;
#endif
  int nb_channels;
  int bit_rate;
  int frame_size;
  enum AVSampleFormat sample_format;
  bool with_wav_header;
  bool without_libav_header;

  // Video settings
  enum AVCodecID video_codec;
  enum AVPixelFormat pix_fmt;
  int height;
  int width;
};

struct stream_ctx
{
  AVStream *stream;
  AVCodecContext *codec;

  AVFilterContext *buffersink_ctx;
  AVFilterContext *buffersrc_ctx;
  AVFilterGraph *filter_graph;

  // Used for seeking
  int64_t prev_pts;
  int64_t offset_pts;
};

struct decode_ctx
{
  // Settings derived from the profile
  struct settings_ctx settings;

  // Input format context (used as a container for stream info even in raw mode)
  AVFormatContext *ifmt_ctx;

  // IO Context for non-file input
  AVIOContext *avio;

  // Stream and decoder data
  struct stream_ctx audio_stream;
  struct stream_ctx video_stream;

  // Source duration in ms as provided by caller
  uint32_t len_ms;
};

struct encode_ctx
{
  // Settings derived from the profile
  struct settings_ctx settings;

  // Output format context
  AVFormatContext *ofmt_ctx;

  // Stream, filter and decoder data
  struct stream_ctx audio_stream;
  struct stream_ctx video_stream;

  // The ffmpeg muxer writes to this buffer using the avio_evbuffer interface
  struct evbuffer *obuf;

  // IO Context for non-file output
  struct transcode_evbuf_io evbuf_io;

  // Contains the most recent packet from av_buffersink_get_frame()
  AVFrame *filt_frame;

  // Contains the most recent packet from avcodec_receive_packet()
  AVPacket *encoded_pkt;

  // How many output bytes we have processed in total
  off_t bytes_processed;

  // Estimated total size of output
  off_t bytes_total;


};

struct avio_evbuffer {
  struct evbuffer *evbuf;
  uint8_t *buffer;
  transcode_seekfn seekfn;
  void *seekfn_arg;
};

struct filter_def
{
  char name[64];
  char args[512];
};

struct filters
{
  AVFilterContext *av_ctx;

  // Function that will create the filter arguments for ffmpeg
  int (*deffn)(struct filter_def *, struct stream_ctx *, struct stream_ctx *, const char *);
  const char *deffn_arg;
};


/* -------------------------- PROFILE CONFIGURATION ------------------------ */

static int
init_settings(struct settings_ctx *settings, enum transcode_profile profile, struct media_quality *quality)
{
  memset(settings, 0, sizeof(struct settings_ctx));

  switch (profile)
    {
      case XCODE_PCM16:
	settings->encode_audio = true;
	settings->format = "s16le";
	settings->audio_codec = AV_CODEC_ID_PCM_S16LE;
	settings->sample_format = AV_SAMPLE_FMT_S16;
	break;

      case XCODE_ALAC:
	settings->encode_audio = true;
	settings->format = "data"; // Means we get the raw packet from the encoder, no muxing
	settings->audio_codec = AV_CODEC_ID_ALAC;
	settings->sample_format = AV_SAMPLE_FMT_S16P;
	settings->frame_size = 352;
	break;

      default:
	DPRINTF(E_LOG, L_XCODE, "Bug! Unknown transcoding profile\n");
	return -1;
    }

  if (quality && quality->sample_rate)
    {
      settings->sample_rate    = quality->sample_rate;
    }

  if (quality && quality->channels)
    {
#if USE_CH_LAYOUT
      av_channel_layout_default(&settings->channel_layout, quality->channels);
#else
      settings->channel_layout = av_get_default_channel_layout(quality->channels);
      settings->nb_channels    = quality->channels;
#endif
    }

  if (quality && quality->bit_rate)
    {
      settings->bit_rate    = quality->bit_rate;
    }

  if (quality && quality->bits_per_sample && (quality->bits_per_sample != 8 * av_get_bytes_per_sample(settings->sample_format)))
    {
      DPRINTF(E_LOG, L_XCODE, "Bug! Mismatch between profile (%d bps) and media quality (%d bps)\n", 8 * av_get_bytes_per_sample(settings->sample_format), quality->bits_per_sample);
      return -1;
    }

  return 0;
}

static int
init_settings_from_video(struct settings_ctx *settings, enum transcode_profile profile, struct decode_ctx *src_ctx, int width, int height)
{
  settings->width = width;
  settings->height = height;

  return 0;
}

static int
init_settings_from_audio(struct settings_ctx *settings, enum transcode_profile profile, struct decode_ctx *src_ctx, struct media_quality *quality)
{
  // Initialize unset settings that are source-dependent, not profile-dependent
  if (!settings->sample_rate)
    settings->sample_rate = src_ctx->audio_stream.codec->sample_rate;

#if USE_CH_LAYOUT
  if (!av_channel_layout_check(&settings->channel_layout))
    av_channel_layout_copy(&settings->channel_layout, &src_ctx->audio_stream.codec->ch_layout);

  settings->nb_channels = settings->channel_layout.nb_channels;
#else
  if (settings->nb_channels == 0)
    {
      settings->nb_channels = src_ctx->audio_stream.codec->channels;
      settings->channel_layout = src_ctx->audio_stream.codec->channel_layout;
    }
#endif

  // Verify that all required encoding parameters are set by init_settings()
  if (!settings->sample_format || !settings->audio_codec || !settings->format)
    {
      DPRINTF(E_LOG, L_XCODE, "Bug! Profile %d has unset encoding parameters\n", profile);
      return -1;
    }

  return 0;
}

static void
stream_settings_set(struct stream_ctx *s, struct settings_ctx *settings, enum AVMediaType type)
{
  if (type == AVMEDIA_TYPE_AUDIO)
    {
      s->codec->sample_rate    = settings->sample_rate;
#if USE_CH_LAYOUT
      av_channel_layout_copy(&s->codec->ch_layout, &(settings->channel_layout));
#else
      s->codec->channel_layout = settings->channel_layout;
      s->codec->channels       = settings->nb_channels;
#endif
      s->codec->sample_fmt     = settings->sample_format;
      s->codec->time_base      = (AVRational){1, settings->sample_rate};
      s->codec->bit_rate       = settings->bit_rate;
    }
  else if (type == AVMEDIA_TYPE_VIDEO)
    {
      s->codec->height         = settings->height;
      s->codec->width          = settings->width;
      s->codec->pix_fmt        = settings->pix_fmt;
      s->codec->time_base      = (AVRational){1, 25};
    }
}


/* -------------------------------- HELPERS -------------------------------- */

static enum AVSampleFormat
bitdepth2format(int bits_per_sample)
{
  if (bits_per_sample == 16)
    return AV_SAMPLE_FMT_S16;
  else if (bits_per_sample == 24)
    return AV_SAMPLE_FMT_S32;
  else if (bits_per_sample == 32)
    return AV_SAMPLE_FMT_S32;
  else
    return AV_SAMPLE_FMT_NONE;
}

static inline char *
err2str(int errnum)
{
  av_strerror(errnum, errbuf, sizeof(errbuf));
  return errbuf;
}

static inline void
add_le16(uint8_t *dst, uint16_t val)
{
  dst[0] = val & 0xff;
  dst[1] = (val >> 8) & 0xff;
}

static inline void
add_le32(uint8_t *dst, uint32_t val)
{
  dst[0] = val & 0xff;
  dst[1] = (val >> 8) & 0xff;
  dst[2] = (val >> 16) & 0xff;
  dst[3] = (val >> 24) & 0xff;
}

static off_t
size_estimate(enum transcode_profile profile, uint32_t bit_rate, uint32_t sample_rate, uint16_t bytes_per_sample, uint16_t channels, uint32_t len_ms)
{
  return 0;
}


/*
 * Adds a stream to an output
 *
 * @out ctx       A pre-allocated stream ctx where we save stream and codec info
 * @in output     Output to add the stream to
 * @in codec_id   What kind of codec should we use
 * @return        Negative on failure, otherwise zero
 */
static int
stream_add(struct encode_ctx *ctx, struct stream_ctx *s, enum AVCodecID codec_id)
{
  const AVCodecDescriptor *codec_desc;
#if USE_CONST_AVCODEC
  const AVCodec *encoder;
#else
  // Not const before ffmpeg 5.0
  AVCodec *encoder;
#endif
  AVDictionary *options = NULL;
  const enum AVPixelFormat *pix_fmts = NULL;
  int ret;

  codec_desc = avcodec_descriptor_get(codec_id);
  if (!codec_desc)
    {
      DPRINTF(E_LOG, L_XCODE, "Invalid codec ID (%d)\n", codec_id);
      return -1;
    }

  encoder = avcodec_find_encoder(codec_id);
  if (!encoder)
    {
      DPRINTF(E_LOG, L_XCODE, "Necessary encoder (%s) not found\n", codec_desc->name);
      return -1;
    }

  DPRINTF(E_DBG, L_XCODE, "Selected encoder '%s'\n", encoder->long_name);

  CHECK_NULL(L_XCODE, s->stream = avformat_new_stream(ctx->ofmt_ctx, NULL));
  CHECK_NULL(L_XCODE, s->codec = avcodec_alloc_context3(encoder));

  stream_settings_set(s, &ctx->settings, encoder->type);

  if (!s->codec->pix_fmt)
    {
#if USE_AVCODEC_GET_SUPPORTED_CONFIG
      avcodec_get_supported_config(s->codec, NULL, AV_CODEC_CONFIG_PIX_FORMAT, 0, (const void **)&pix_fmts, NULL);
#else
      pix_fmts = encoder->pix_fmts;
#endif
      s->codec->pix_fmt = pix_fmts ? avcodec_default_get_format(s->codec, pix_fmts) : AV_PIX_FMT_NONE;
      DPRINTF(E_DBG, L_XCODE, "Pixel format set to %s (encoder is %s)\n", av_get_pix_fmt_name(s->codec->pix_fmt), codec_desc->name);
    }

  if (ctx->ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
    s->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  // With ffmpeg 3.4, jpeg encoding with optimal huffman tables will segfault, see issue #502
  if (codec_id == AV_CODEC_ID_MJPEG)
    av_dict_set(&options, "huffman", "default", 0);

  // 20 ms frames is the current ffmpeg default, but we set it anyway, so that
  // we don't risk issues if future versions change the default (it would become
  // an issue because outputs/cast.c relies on 20 ms frames)
  if (codec_id == AV_CODEC_ID_OPUS)
    av_dict_set(&options, "frame_duration", "20", 0);

  ret = avcodec_open2(s->codec, NULL, &options);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Cannot open encoder (%s): %s\n", codec_desc->name, err2str(ret));
      goto error;
    }

  // airplay.c "misuses" the ffmpeg alac encoder in that it pushes frames with
  // 352 samples even though the encoder wants 4096 (and doesn't have variable
  // frame capability). This worked with no issues until ffmpeg 6, where it
  // seems a frame size check was added. The below circumvents the check, but is
  // dirty because we shouldn't be writing to this data element.
  if (ctx->settings.frame_size)
    s->codec->frame_size = ctx->settings.frame_size;

  // Copy the codec parameters we just set to the stream, so the muxer knows them
  ret = avcodec_parameters_from_context(s->stream->codecpar, s->codec);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Cannot copy stream parameters (%s): %s\n", codec_desc->name, err2str(ret));
      goto error;
    }

  if (options)
    {
      DPRINTF(E_WARN, L_XCODE, "Encoder %s didn't recognize all options given to avcodec_open2\n", codec_desc->name);
      av_dict_free(&options);
    }

  return 0;

 error:
  if (s->codec)
    avcodec_free_context(&s->codec);
  if (options)
    av_dict_free(&options);

  return -1;
}

// Prepares a packet from the encoder for muxing
static void
packet_prepare(AVPacket *pkt, struct stream_ctx *s)
{
  pkt->stream_index = s->stream->index;

  // This "wonderful" peace of code makes sure that the timestamp always increases,
  // even if the user seeked backwards. The muxer will not accept non-increasing
  // timestamps.
  pkt->pts += s->offset_pts;
  if (pkt->pts < s->prev_pts)
    {
      s->offset_pts += s->prev_pts - pkt->pts;
      pkt->pts = s->prev_pts;
    }
  s->prev_pts = pkt->pts;
  pkt->dts = pkt->pts; //FIXME

  av_packet_rescale_ts(pkt, s->codec->time_base, s->stream->time_base);
}

/*
 * Part 4+5 of the conversion chain: read -> decode -> filter -> encode -> write
 *
 */
static int
encode_write(struct encode_ctx *ctx, struct stream_ctx *s, AVFrame *filt_frame)
{
  int ret;

  // If filt_frame is null then flushing will be initiated by the codec
  ret = avcodec_send_frame(s->codec, filt_frame);
  if (ret < 0)
    return ret;

  while (1)
    {
      ret = avcodec_receive_packet(s->codec, ctx->encoded_pkt);
      if (ret < 0)
	{
	  if (ret == AVERROR(EAGAIN))
	    ret = 0;

	  break;
	}

      packet_prepare(ctx->encoded_pkt, s);

      ret = av_interleaved_write_frame(ctx->ofmt_ctx, ctx->encoded_pkt);
      if (ret < 0)
        {
	  DPRINTF(E_WARN, L_XCODE, "av_interleaved_write_frame() failed: %s\n", err2str(ret));
	  break;
        }
    }

  return ret;
}

/*
 * Part 3 of the conversion chain: read -> decode -> filter -> encode -> write
 *
 * transcode_encode() starts here since the caller already has a frame
 *
 */
static int
filter_encode_write(struct encode_ctx *ctx, struct stream_ctx *s, AVFrame *frame)
{
  int ret;

  // Push the decoded frame into the filtergraph. If frame is NULL then it
  // signals EOF to ffmpeg which is necessary to make av_buffersink_get_frame
  // return the last frames (see issue #1787)
  ret = av_buffersrc_add_frame(s->buffersrc_ctx, frame);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Error while feeding the filtergraph: %s\n", err2str(ret));
      return -1;
    }

  // Pull filtered frames from the filtergraph and pass to encoder
  while (1)
    {
      ret = av_buffersink_get_frame(s->buffersink_ctx, ctx->filt_frame);
      if (ret < 0)
	{
	  if (!frame) // We are flushing
	    ret = encode_write(ctx, s, NULL);
	  else if (ret == AVERROR(EAGAIN))
	    ret = 0;

	  break;
	}

      ret = encode_write(ctx, s, ctx->filt_frame);
      av_frame_unref(ctx->filt_frame);
      if (ret < 0)
	break;
    }

  return ret;
}

/* ------------------------------- CUSTOM I/O ------------------------------ */
/*      For using ffmpeg with evbuffer input/output instead of files         */

static int
avio_evbuffer_read(void *opaque, uint8_t *buf, int size)
{
  struct avio_evbuffer *ae = (struct avio_evbuffer *)opaque;
  int ret;

  ret = evbuffer_remove(ae->evbuf, buf, size);

  // Must return AVERROR, see avio.h: avio_alloc_context()
  return (ret > 0) ? ret : AVERROR_EOF;
}

#if USE_CONST_AVIO_WRITE_PACKET
static int
avio_evbuffer_write(void *opaque, const uint8_t *buf, int size)
#else
static int
avio_evbuffer_write(void *opaque, uint8_t *buf, int size)
#endif
{
  struct avio_evbuffer *ae = (struct avio_evbuffer *)opaque;
  int ret;

  ret = evbuffer_add(ae->evbuf, buf, size);

  return (ret == 0) ? size : -1;
}

static int64_t
avio_evbuffer_seek(void *opaque, int64_t offset, int whence)
{
  struct avio_evbuffer *ae = (struct avio_evbuffer *)opaque;
  enum transcode_seek_type seek_type;

  // Caller shouldn't need to know about ffmpeg defines
  if (whence & AVSEEK_SIZE)
    seek_type = XCODE_SEEK_SIZE;
  else if (whence == SEEK_SET)
    seek_type = XCODE_SEEK_SET;
  else if (whence == SEEK_CUR)
    seek_type = XCODE_SEEK_CUR;
  else
    return -1;

  return ae->seekfn(ae->seekfn_arg, offset, seek_type);
}

static AVIOContext *
avio_evbuffer_open(struct transcode_evbuf_io *evbuf_io, int is_output)
{
  struct avio_evbuffer *ae;
  AVIOContext *s;

  ae = calloc(1, sizeof(struct avio_evbuffer));
  if (!ae)
    {
      DPRINTF(E_LOG, L_FFMPEG, "Out of memory for avio_evbuffer\n");

      return NULL;
    }

  ae->buffer = av_mallocz(AVIO_BUFFER_SIZE);
  if (!ae->buffer)
    {
      DPRINTF(E_LOG, L_FFMPEG, "Out of memory for avio buffer\n");

      free(ae);
      return NULL;
    }

  ae->evbuf = evbuf_io->evbuf;
  ae->seekfn = evbuf_io->seekfn;
  ae->seekfn_arg = evbuf_io->seekfn_arg;

  if (is_output)
    s = avio_alloc_context(ae->buffer, AVIO_BUFFER_SIZE, 1, ae, NULL, avio_evbuffer_write, (evbuf_io->seekfn ? avio_evbuffer_seek : NULL));
  else
    s = avio_alloc_context(ae->buffer, AVIO_BUFFER_SIZE, 0, ae, avio_evbuffer_read, NULL, (evbuf_io->seekfn ? avio_evbuffer_seek : NULL));

  if (!s)
    {
      DPRINTF(E_LOG, L_FFMPEG, "Could not allocate AVIOContext\n");

      av_free(ae->buffer);
      free(ae);
      return NULL;
    }

  s->seekable = (evbuf_io->seekfn ? AVIO_SEEKABLE_NORMAL : 0);

  return s;
}

static void
avio_evbuffer_close(AVIOContext *s)
{
  struct avio_evbuffer *ae;

  if (!s)
    return;

  ae = (struct avio_evbuffer *)s->opaque;

  avio_flush(s);

  av_free(s->buffer);
  free(ae);

  av_free(s);
}


/* ----------------------- CUSTOM HEADER GENERATION ------------------------ */

static int
make_wav_header(struct evbuffer **wav_header, uint32_t sample_rate, uint16_t bytes_per_sample, uint16_t channels, uint32_t bytes_total)
{
  uint8_t header[WAV_HEADER_LEN];

  memcpy(header, "RIFF", 4);
  add_le32(header + 4, bytes_total - 8); // Total file size - 8 bytes as defined by the format
  memcpy(header + 8, "WAVEfmt ", 8);
  add_le32(header + 16, 16);
  add_le16(header + 20, 1); // AudioFormat (PCM)
  add_le16(header + 22, channels);     /* channels */
  add_le32(header + 24, sample_rate);  /* samplerate */
  add_le32(header + 28, sample_rate * channels * bytes_per_sample); /* byte rate */
  add_le16(header + 32, channels * bytes_per_sample);               /* block align */
  add_le16(header + 34, 8 * bytes_per_sample);                      /* bits per sample */
  memcpy(header + 36, "data", 4);
  add_le32(header + 40, bytes_total - WAV_HEADER_LEN);

  *wav_header = evbuffer_new();
  evbuffer_add(*wav_header, header, sizeof(header));
  return 0;
}

/* --------------------------- INPUT/OUTPUT INIT --------------------------- */

static void
close_input(struct decode_ctx *ctx)
{
  if (!ctx->ifmt_ctx)
    return;

  avio_evbuffer_close(ctx->avio);
  avcodec_free_context(&ctx->audio_stream.codec);
  avcodec_free_context(&ctx->video_stream.codec);
  avformat_close_input(&ctx->ifmt_ctx);
  ctx->ifmt_ctx = NULL;
}

static void
close_output(struct encode_ctx *ctx)
{
  if (!ctx->ofmt_ctx)
    return;

  avcodec_free_context(&ctx->audio_stream.codec);
  avcodec_free_context(&ctx->video_stream.codec);
  avio_evbuffer_close(ctx->ofmt_ctx->pb);
  avformat_free_context(ctx->ofmt_ctx);
  ctx->ofmt_ctx = NULL;
}

static int
open_output(struct encode_ctx *ctx, struct transcode_evbuf_io *evbuf_io, struct evbuffer *prepared_header, struct decode_ctx *src_ctx)
{
#if USE_CONST_AVFORMAT
  const AVOutputFormat *oformat;
#else
  // Not const before ffmpeg 5.0
  AVOutputFormat *oformat;
#endif
  AVDictionary *options = NULL;
  struct evbuffer *header = NULL;
  int ret;

  oformat = av_guess_format(ctx->settings.format, NULL, NULL);
  if (!oformat)
    {
      DPRINTF(E_LOG, L_XCODE, "ffmpeg/libav could not find the '%s' output format\n", ctx->settings.format);
      return -1;
    }

#if USE_NO_CLEAR_AVFMT_NOFILE
  CHECK_ERRNO(L_XCODE, avformat_alloc_output_context2(&ctx->ofmt_ctx, oformat, NULL, NULL));
#else
  // Clear AVFMT_NOFILE bit, it is not allowed as we will set our own AVIOContext.
  // If this is not done with e.g. ffmpeg 3.4 then artwork rescaling will fail.
  oformat->flags &= ~AVFMT_NOFILE;

  CHECK_NULL(L_XCODE, ctx->ofmt_ctx = avformat_alloc_context());

  ctx->ofmt_ctx->oformat = oformat;
#endif

  CHECK_NULL(L_XCODE, ctx->ofmt_ctx->pb = avio_evbuffer_open(evbuf_io, 1));
  ctx->obuf = evbuf_io->evbuf;

  if (ctx->settings.encode_audio)
    {
      ret = stream_add(ctx, &ctx->audio_stream, ctx->settings.audio_codec);
      if (ret < 0)
	goto error;
    }

  if (ctx->settings.encode_video)
    {
      ret = stream_add(ctx, &ctx->video_stream, ctx->settings.video_codec);
      if (ret < 0)
	goto error;
    }

  ret = avformat_init_output(ctx->ofmt_ctx, &options);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Error initializing output: %s\n", err2str(ret));
      goto error;
    }
  else if (options)
    {
      DPRINTF(E_WARN, L_XCODE, "Didn't recognize all options given to avformat_init_output\n");
      av_dict_free(&options);
      goto error;
    }

  // For WAV output, both avformat_write_header() and manual wav header is required
  if (!ctx->settings.without_libav_header)
    {
      ret = avformat_write_header(ctx->ofmt_ctx, NULL);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Error writing header to output buffer: %s\n", err2str(ret));
	  goto error;
	}
    }
  if (ctx->settings.with_wav_header)
    {
      ret = make_wav_header(&header, ctx->settings.sample_rate, av_get_bytes_per_sample(ctx->settings.sample_format), ctx->settings.nb_channels, ctx->bytes_total);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Error creating WAV header\n");
	  goto error;
	}

      evbuffer_add_buffer(ctx->obuf, header);
      evbuffer_free(header);
    }

  return 0;

 error:
  close_output(ctx);
  return -1;
}

static int
filter_def_abuffer(struct filter_def *def, struct stream_ctx *out_stream, struct stream_ctx *in_stream, const char *deffn_arg)
{
#if USE_CH_LAYOUT
  char buf[64];

  // Some AIFF files only have a channel number, not a layout
  if (in_stream->codec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
    av_channel_layout_default(&in_stream->codec->ch_layout, in_stream->codec->ch_layout.nb_channels);

  av_channel_layout_describe(&in_stream->codec->ch_layout, buf, sizeof(buf));

  snprintf(def->args, sizeof(def->args),
           "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
           in_stream->stream->time_base.num, in_stream->stream->time_base.den,
           in_stream->codec->sample_rate, av_get_sample_fmt_name(in_stream->codec->sample_fmt),
           buf);
#else
  if (!in_stream->codec->channel_layout)
    in_stream->codec->channel_layout = av_get_default_channel_layout(in_stream->codec->channels);

  snprintf(def->args, sizeof(def->args),
           "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64,
           in_stream->stream->time_base.num, in_stream->stream->time_base.den,
           in_stream->codec->sample_rate, av_get_sample_fmt_name(in_stream->codec->sample_fmt),
           in_stream->codec->channel_layout);
#endif
  snprintf(def->name, sizeof(def->name), "abuffer");
  return 0;
}

static int
filter_def_aformat(struct filter_def *def, struct stream_ctx *out_stream, struct stream_ctx *in_stream, const char *deffn_arg)
{
#if USE_CH_LAYOUT
  char buf[64];

  if (out_stream->codec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
    av_channel_layout_default(&out_stream->codec->ch_layout, out_stream->codec->ch_layout.nb_channels);

  av_channel_layout_describe(&out_stream->codec->ch_layout, buf, sizeof(buf));

  snprintf(def->args, sizeof(def->args),
           "sample_fmts=%s:sample_rates=%d:channel_layouts=%s",
           av_get_sample_fmt_name(out_stream->codec->sample_fmt), out_stream->codec->sample_rate,
           buf);
#else
  // For some AIFF files, ffmpeg (3.4.6) will not give us a channel_layout (bug in ffmpeg?)
  if (!out_stream->codec->channel_layout)
    out_stream->codec->channel_layout = av_get_default_channel_layout(out_stream->codec->channels);

  snprintf(def->args, sizeof(def->args),
           "sample_fmts=%s:sample_rates=%d:channel_layouts=0x%"PRIx64,
           av_get_sample_fmt_name(out_stream->codec->sample_fmt), out_stream->codec->sample_rate,
           out_stream->codec->channel_layout);
#endif
  snprintf(def->name, sizeof(def->name), "aformat");
  return 0;
}

static int
filter_def_abuffersink(struct filter_def *def, struct stream_ctx *out_stream, struct stream_ctx *in_stream, const char *deffn_arg)
{
  snprintf(def->name, sizeof(def->name), "abuffersink");
  *def->args = '\0';
  return 0;
}

static int
filter_def_buffer(struct filter_def *def, struct stream_ctx *out_stream, struct stream_ctx *in_stream, const char *deffn_arg)
{
  snprintf(def->name, sizeof(def->name), "buffer");
  snprintf(def->args, sizeof(def->args),
           "width=%d:height=%d:pix_fmt=%s:time_base=%d/%d:sar=%d/%d",
           in_stream->codec->width, in_stream->codec->height, av_get_pix_fmt_name(in_stream->codec->pix_fmt),
           in_stream->stream->time_base.num, in_stream->stream->time_base.den,
           in_stream->codec->sample_aspect_ratio.num, in_stream->codec->sample_aspect_ratio.den);
  return 0;
}

static int
filter_def_format(struct filter_def *def, struct stream_ctx *out_stream, struct stream_ctx *in_stream, const char *deffn_arg)
{
  snprintf(def->name, sizeof(def->name), "format");
  snprintf(def->args, sizeof(def->args),
           "pix_fmts=%s", av_get_pix_fmt_name(out_stream->codec->pix_fmt));
  return 0;
}

static int
filter_def_scale(struct filter_def *def, struct stream_ctx *out_stream, struct stream_ctx *in_stream, const char *deffn_arg)
{
  snprintf(def->name, sizeof(def->name), "scale");
  snprintf(def->args, sizeof(def->args),
           "w=%d:h=%d", out_stream->codec->width, out_stream->codec->height);
  return 0;
}

static int
filter_def_buffersink(struct filter_def *def, struct stream_ctx *out_stream, struct stream_ctx *in_stream, const char *deffn_arg)
{
  snprintf(def->name, sizeof(def->name), "buffersink");
  *def->args = '\0';
  return 0;
}

static int
define_audio_filters(struct filters *filters, size_t filters_len)
{
  filters[0].deffn = filter_def_abuffer;
  filters[1].deffn = filter_def_aformat;
  filters[2].deffn = filter_def_abuffersink;

  return 0;
}

static int
define_video_filters(struct filters *filters, size_t filters_len)
{
  filters[0].deffn = filter_def_buffer;
  filters[1].deffn = filter_def_format;
  filters[2].deffn = filter_def_scale;
  filters[3].deffn = filter_def_buffersink;

  return 0;
}

static int
add_filters(int *num_added, AVFilterGraph *filter_graph, struct filters *filters, size_t filters_len,
            struct stream_ctx *out_stream, struct stream_ctx *in_stream)
{
  const AVFilter *av_filter;
  struct filter_def def;
  int i;
  int ret;

  for (i = 0; i < filters_len && filters[i].deffn; i++)
    {
      ret = filters[i].deffn(&def, out_stream, in_stream, filters[i].deffn_arg);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Error creating filter definition\n");
	  return -1;
	}

      av_filter = avfilter_get_by_name(def.name);
      if (!av_filter)
	{
	  DPRINTF(E_LOG, L_XCODE, "Could not find filter '%s'\n", def.name);
	  return -1;
	}

      ret = avfilter_graph_create_filter(&filters[i].av_ctx, av_filter, def.name, def.args, NULL, filter_graph);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Error creating filter '%s': %s\n", def.name, err2str(ret));
	  return -1;
	}

      DPRINTF(E_DBG, L_XCODE, "Created '%s' filter: '%s'\n", def.name, def.args);

      if (i == 0)
	continue;

      ret = avfilter_link(filters[i - 1].av_ctx, 0, filters[i].av_ctx, 0);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Error connecting filters: %s\n", err2str(ret));
	  return -1;
	}
    }

  *num_added = i;
  return 0;
}

static int
create_filtergraph(struct stream_ctx *out_stream, struct filters *filters, size_t filters_len, struct stream_ctx *in_stream)
{
  AVFilterGraph *filter_graph;
  int ret;
  int added;

  CHECK_NULL(L_XCODE, filter_graph = avfilter_graph_alloc());

  ret = add_filters(&added, filter_graph, filters, filters_len, out_stream, in_stream);
  if (ret < 0)
    {
      goto out_fail;
    }

  ret = avfilter_graph_config(filter_graph, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Filter graph config failed: %s\n", err2str(ret));
      goto out_fail;
    }

  out_stream->buffersrc_ctx = filters[0].av_ctx;
  out_stream->buffersink_ctx = filters[added - 1].av_ctx;
  out_stream->filter_graph = filter_graph;

  return 0;

 out_fail:
  avfilter_graph_free(&filter_graph);
  return -1;
}

static void
close_filters(struct encode_ctx *ctx)
{
  avfilter_graph_free(&ctx->audio_stream.filter_graph);
  avfilter_graph_free(&ctx->video_stream.filter_graph);
}

static int
open_filters(struct encode_ctx *ctx, struct decode_ctx *src_ctx)
{
  struct filters filters[MAX_FILTERS] = { 0 };
  int ret;

  if (ctx->settings.encode_audio)
    {
      ret = define_audio_filters(filters, ARRAY_SIZE(filters));
      if (ret < 0)
	goto out_fail;

      ret = create_filtergraph(&ctx->audio_stream, filters, ARRAY_SIZE(filters), &src_ctx->audio_stream);
      if (ret < 0)
	goto out_fail;

      // Many audio encoders require a fixed frame size. This will ensure that
      // the filt_frame from av_buffersink_get_frame has that size (except EOF).
      if (! (ctx->audio_stream.codec->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE))
	av_buffersink_set_frame_size(ctx->audio_stream.buffersink_ctx, ctx->audio_stream.codec->frame_size);
    }

  if (ctx->settings.encode_video)
    {
      ret = define_video_filters(filters, ARRAY_SIZE(filters));
      if (ret < 0)
	goto out_fail;

      ret = create_filtergraph(&ctx->video_stream, filters, ARRAY_SIZE(filters), &src_ctx->video_stream);
      if (ret < 0)
	goto out_fail;
    }

  return 0;

 out_fail:
  close_filters(ctx);
  return -1;
}


/* ----------------------------- TRANSCODE API ----------------------------- */

/*                                  Setup                                    */

struct encode_ctx *
transcode_encode_setup(struct transcode_encode_setup_args args)
{
  struct encode_ctx *ctx;
  int dst_bytes_per_sample;

  CHECK_NULL(L_XCODE, ctx = calloc(1, sizeof(struct encode_ctx)));
  CHECK_NULL(L_XCODE, ctx->filt_frame = av_frame_alloc());
  CHECK_NULL(L_XCODE, ctx->encoded_pkt = av_packet_alloc());
  CHECK_NULL(L_XCODE, ctx->evbuf_io.evbuf = evbuffer_new());

  // Caller didn't specify one, so use our own
  if (!args.evbuf_io)
    args.evbuf_io = &ctx->evbuf_io;

  // Initialize general settings
  if (init_settings(&ctx->settings, args.profile, args.quality) < 0)
    goto error;

  if (ctx->settings.encode_audio && init_settings_from_audio(&ctx->settings, args.profile, args.src_ctx, args.quality) < 0)
    goto error;

  if (ctx->settings.encode_video && init_settings_from_video(&ctx->settings, args.profile, args.src_ctx, args.width, args.height) < 0)
    goto error;

  dst_bytes_per_sample = av_get_bytes_per_sample(ctx->settings.sample_format);
  ctx->bytes_total = size_estimate(args.profile, ctx->settings.bit_rate, ctx->settings.sample_rate, dst_bytes_per_sample, ctx->settings.nb_channels, args.src_ctx->len_ms);

  if (open_output(ctx, args.evbuf_io, args.prepared_header, args.src_ctx) < 0)
    goto error;

  if (open_filters(ctx, args.src_ctx) < 0)
    goto error;

  return ctx;

 error:
  transcode_encode_cleanup(&ctx);
  return NULL;
}

struct decode_ctx *
transcode_decode_setup_raw(enum transcode_profile profile, struct media_quality *quality)
{
  const AVCodecDescriptor *codec_desc;
  struct decode_ctx *ctx;
#if USE_CONST_AVCODEC
  const AVCodec *decoder;
#else
  // Not const before ffmpeg 5.0
  AVCodec *decoder;
#endif
  int ret;

  CHECK_NULL(L_XCODE, ctx = calloc(1, sizeof(struct decode_ctx)));

  if (init_settings(&ctx->settings, profile, quality) < 0)
    {
      goto out_free_ctx;
    }

  codec_desc = avcodec_descriptor_get(ctx->settings.audio_codec);
  if (!codec_desc)
    {
      DPRINTF(E_LOG, L_XCODE, "Invalid codec ID (%d)\n", ctx->settings.audio_codec);
      goto out_free_ctx;
    }

  // In raw mode we won't actually need to read or decode, but we still setup
  // the decode_ctx because transcode_encode_setup() gets info about the input
  // through this structure (TODO dont' do that)
  decoder = avcodec_find_decoder(ctx->settings.audio_codec);
  if (!decoder)
    {
      DPRINTF(E_LOG, L_XCODE, "Could not find decoder for: %s\n", codec_desc->name);
      goto out_free_ctx;
    }

  CHECK_NULL(L_XCODE, ctx->ifmt_ctx = avformat_alloc_context());
  CHECK_NULL(L_XCODE, ctx->audio_stream.codec = avcodec_alloc_context3(decoder));
  CHECK_NULL(L_XCODE, ctx->audio_stream.stream = avformat_new_stream(ctx->ifmt_ctx, NULL));

  stream_settings_set(&ctx->audio_stream, &ctx->settings, decoder->type);

  // Copy the data we just set to the structs we will be querying later, e.g. in open_filter
  ctx->audio_stream.stream->time_base = ctx->audio_stream.codec->time_base;
  ret = avcodec_parameters_from_context(ctx->audio_stream.stream->codecpar, ctx->audio_stream.codec);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Cannot copy stream parameters (%s): %s\n", codec_desc->name, err2str(ret));
      goto out_free_codec;
    }

  return ctx;

 out_free_codec:
  avcodec_free_context(&ctx->audio_stream.codec);
  avformat_free_context(ctx->ifmt_ctx);
 out_free_ctx:
  free(ctx);
  return NULL;
}

/*                                  Cleanup                                  */

void
transcode_decode_cleanup(struct decode_ctx **ctx)
{
  if (!(*ctx))
    return;

  close_input(*ctx);

  free(*ctx);
  *ctx = NULL;
}

void
transcode_encode_cleanup(struct encode_ctx **ctx)
{
  if (!*ctx)
    return;

  close_filters(*ctx);
  close_output(*ctx);

  evbuffer_free((*ctx)->evbuf_io.evbuf);
  av_packet_free(&(*ctx)->encoded_pkt);
  av_frame_free(&(*ctx)->filt_frame);
  free(*ctx);
  *ctx = NULL;
}

/*                       Encoding                                             */

// Filters and encodes
int
transcode_encode(struct evbuffer *evbuf, struct encode_ctx *ctx, transcode_frame *frame, int eof)
{
  AVFrame *f = frame;
  struct stream_ctx *s;
  size_t start_length;
  int ret;

  start_length = evbuffer_get_length(ctx->obuf);

  // Really crappy way of detecting if frame is audio, video or something else
#if USE_CH_LAYOUT
  if (f->ch_layout.nb_channels && f->sample_rate)
#else
  if (f->channel_layout && f->sample_rate)
#endif
    s = &ctx->audio_stream;
  else if (f->width && f->height)
    s = &ctx->video_stream;
  else
    {
      DPRINTF(E_LOG, L_XCODE, "Bug! Encoder could not detect frame type\n");
      return -1;
    }

  ret = filter_encode_write(ctx, s, f);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Error occurred while encoding: %s\n", err2str(ret));
      return ret;
    }

  // Flush
  if (eof)
    {
      filter_encode_write(ctx, s, NULL);
      av_write_trailer(ctx->ofmt_ctx);
    }

  ret = evbuffer_get_length(ctx->obuf) - start_length;

  evbuffer_add_buffer(evbuf, ctx->obuf);

  return ret;
}

transcode_frame *
transcode_frame_new(void *data, size_t size, int nsamples, struct media_quality *quality)
{
  AVFrame *f;
  int ret;

  f = av_frame_alloc();
  if (!f)
    {
      DPRINTF(E_LOG, L_XCODE, "Out of memory for frame\n");
      return NULL;
    }

  f->format = bitdepth2format(quality->bits_per_sample);
  if (f->format == AV_SAMPLE_FMT_NONE)
    {
      DPRINTF(E_LOG, L_XCODE, "transcode_frame_new() called with unsupported bps (%d)\n", quality->bits_per_sample);
      av_frame_free(&f);
      return NULL;
    }

  f->sample_rate    = quality->sample_rate;
  f->nb_samples     = nsamples;
#if USE_CH_LAYOUT
  av_channel_layout_default(&f->ch_layout, quality->channels);
#else
  f->channel_layout = av_get_default_channel_layout(quality->channels);
# ifdef HAVE_FFMPEG
  f->channels       = quality->channels;
# endif
#endif
  f->pts            = AV_NOPTS_VALUE;

  // We don't align because the frame won't be given directly to the encoder
  // anyway, it will first go through the filter (which might align it...?)
  ret = avcodec_fill_audio_frame(f, quality->channels, f->format, data, size, 1);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Error filling frame with rawbuf, size %zu, samples %d (%d/%d/%d): %s\n",
	size, nsamples, quality->sample_rate, quality->bits_per_sample, quality->channels, err2str(ret));

      av_frame_free(&f);
      return NULL;
    }

  return f;
}

void
transcode_frame_free(transcode_frame *frame)
{
  AVFrame *f = frame;

  av_frame_free(&f);
}


