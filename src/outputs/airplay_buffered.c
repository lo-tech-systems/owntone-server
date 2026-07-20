/*
 * Copyright (C) 2026 James Pearce
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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <gcrypt.h>

#include "airplay_buffered.h"
#include "misc.h"
#include "logger.h"

// Buffered-audio (stream type 103) wire format, per block:
//   [uint16 BE total_len][12B header][ciphertext + 16B tag][8B nonce]
// total_len covers everything, including the 2 length bytes themselves.
// Header: bytes 0-3 = (marker << 31) | (seqnum & 0x7fffff), BE
//         bytes 4-7 = rtptime, BE
//         bytes 8-11 = format selector (as given to airplay_buffered_start), BE
// AAD = header bytes 4-11 (rtptime + format selector), 8 bytes.
// Key = shk, first 32 bytes (see AIRPLAY_BUFFERED_KEY_LEN below).

#define AIRPLAY_BUFFERED_KEY_LEN 32
#define AIRPLAY_BUFFERED_HEADER_LEN 12
#define AIRPLAY_BUFFERED_TAG_LEN 16
#define AIRPLAY_BUFFERED_NONCE_LEN 12
#define AIRPLAY_BUFFERED_NONCE_OFFSET 4 // nonce is 4 zero bytes + 8-byte counter
#define AIRPLAY_BUFFERED_NONCE_CLEAR_LEN (AIRPLAY_BUFFERED_NONCE_LEN - AIRPLAY_BUFFERED_NONCE_OFFSET)

// total_len is a uint16, so a single frame's ciphertext+tag can be at most
// this many bytes
#define AIRPLAY_BUFFERED_MAX_PAYLOAD (UINT16_MAX - 2 - AIRPLAY_BUFFERED_HEADER_LEN - AIRPLAY_BUFFERED_TAG_LEN - AIRPLAY_BUFFERED_NONCE_CLEAR_LEN)

struct airplay_buffered_stream
{
  int fd;
  struct event *writeev;
  struct event *readev;
  struct evbuffer *outbuf;

  gcry_cipher_hd_t cipher_hd;
  uint32_t format_id;
  uint64_t nonce_counter;

  bool failed;
};


/* ------------------------------- Crypto ------------------------------- */

// Mirrors chacha_close()/chacha_open()/chacha_encrypt() in airplay.c, which
// encrypt the realtime ALAC RTP payloads with the same cipher/mode. Kept as
// a private copy here since this module has no session/RTP knowledge and
// the airplay.c helpers are static.

static void
chacha_close(gcry_cipher_hd_t hd)
{
  if (!hd)
    return;

  gcry_cipher_close(hd);
}

static gcry_cipher_hd_t
chacha_open(const uint8_t *key, size_t key_len)
{
  gcry_cipher_hd_t hd;

  if (gcry_cipher_open(&hd, GCRY_CIPHER_CHACHA20, GCRY_CIPHER_MODE_POLY1305, 0) != GPG_ERR_NO_ERROR)
    goto error;

  if (gcry_cipher_setkey(hd, key, key_len) != GPG_ERR_NO_ERROR)
    goto error;

  return hd;

 error:
  chacha_close(hd);
  return NULL;
}

static int
chacha_encrypt(uint8_t *cipher, uint8_t *plain, size_t plain_len, const void *ad, size_t ad_len, uint8_t *tag, size_t tag_len, uint8_t *nonce, size_t nonce_len, gcry_cipher_hd_t hd)
{
  if (gcry_cipher_setiv(hd, nonce, nonce_len) != GPG_ERR_NO_ERROR)
    return -1;

  if (gcry_cipher_authenticate(hd, ad, ad_len) != GPG_ERR_NO_ERROR)
    return -1;

  if (gcry_cipher_encrypt(hd, cipher, plain_len, plain, plain_len) != GPG_ERR_NO_ERROR)
    return -1;

  if (gcry_cipher_gettag(hd, tag, tag_len) != GPG_ERR_NO_ERROR)
    return -1;

  return 0;
}


/* --------------------------- Framing/queueing -------------------------- */

// Builds one wire block (length prefix + header + ciphertext + tag + nonce)
// and appends it to bs->outbuf. Like airplay.c's packet_encrypt(), the
// per-packet nonce is 4 zero bytes followed by an 8-byte counter copied in
// host byte order (not explicitly byte-swapped); only the header fields
// (seqnum/rtptime/format selector) that go out on the wire are forced BE.
static int
frame_encrypt_and_queue(struct airplay_buffered_stream *bs, const uint8_t *frame, size_t len, uint32_t seqnum, uint32_t rtptime, bool marker)
{
  uint8_t header[AIRPLAY_BUFFERED_HEADER_LEN];
  uint8_t nonce[AIRPLAY_BUFFERED_NONCE_LEN] = { 0 };
  uint8_t tag[AIRPLAY_BUFFERED_TAG_LEN];
  uint8_t *cipher;
  uint8_t len_be[2];
  uint16_t total_len;
  uint32_t seq_field;
  int ret;

  if (len > AIRPLAY_BUFFERED_MAX_PAYLOAD)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "AirPlay buffered frame too large (%zu bytes)\n", len);
      return -1;
    }

  seq_field = (marker ? 0x80000000 : 0) | (seqnum & 0x7fffff);

  header[0] = (seq_field >> 24) & 0xff;
  header[1] = (seq_field >> 16) & 0xff;
  header[2] = (seq_field >> 8) & 0xff;
  header[3] = seq_field & 0xff;

  header[4] = (rtptime >> 24) & 0xff;
  header[5] = (rtptime >> 16) & 0xff;
  header[6] = (rtptime >> 8) & 0xff;
  header[7] = rtptime & 0xff;

  header[8] = (bs->format_id >> 24) & 0xff;
  header[9] = (bs->format_id >> 16) & 0xff;
  header[10] = (bs->format_id >> 8) & 0xff;
  header[11] = bs->format_id & 0xff;

  // Nonce: 4 zero bytes + 8-byte counter, host byte order (mirrors
  // packet_encrypt() in airplay.c)
  memcpy(nonce + AIRPLAY_BUFFERED_NONCE_OFFSET, &bs->nonce_counter, sizeof(bs->nonce_counter));

  cipher = malloc(len);
  if (!cipher)
    return -1;

  // AAD = rtptime + format selector, i.e. header bytes 4-11
  ret = chacha_encrypt(cipher, (uint8_t *)frame, len, header + 4, 8, tag, sizeof(tag), nonce, sizeof(nonce), bs->cipher_hd);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not encrypt AirPlay buffered audio frame\n");
      free(cipher);
      return -1;
    }

  bs->nonce_counter++;

  total_len = (uint16_t)(2 + AIRPLAY_BUFFERED_HEADER_LEN + len + AIRPLAY_BUFFERED_TAG_LEN + AIRPLAY_BUFFERED_NONCE_CLEAR_LEN);
  len_be[0] = (total_len >> 8) & 0xff;
  len_be[1] = total_len & 0xff;

  evbuffer_add(bs->outbuf, len_be, sizeof(len_be));
  evbuffer_add(bs->outbuf, header, sizeof(header));
  evbuffer_add(bs->outbuf, cipher, len);
  evbuffer_add(bs->outbuf, tag, sizeof(tag));
  evbuffer_add(bs->outbuf, nonce + AIRPLAY_BUFFERED_NONCE_OFFSET, AIRPLAY_BUFFERED_NONCE_CLEAR_LEN);

  free(cipher);

  return 0;
}


/* -------------------------------- Socket -------------------------------- */

static void
write_cb(int fd, short what, void *arg)
{
  struct airplay_buffered_stream *bs = arg;
  int ret;

  ret = evbuffer_write(bs->outbuf, fd);
  if (ret < 0)
    {
      if (errno == EAGAIN || errno == EINTR)
	{
	  event_add(bs->writeev, NULL);
	  return;
	}

      DPRINTF(E_LOG, L_AIRPLAY, "Write error on AirPlay buffered audio socket: %s\n", strerror(errno));
      bs->failed = true;
      return;
    }

  if (evbuffer_get_length(bs->outbuf) > 0)
    event_add(bs->writeev, NULL);
}


// The buffered data connection is a plain TCP socket the receiver could in
// principle write back on (e.g. to signal flow control or an error), even
// though this module's protocol only requires us to send. We keep a
// persistent read event armed so any such inbound bytes are logged rather
// than silently accumulating unread in the kernel socket buffer; we do not
// parse or act on them since the wire protocol here defines no meaningful
// receiver-to-sender message.
static void
read_cb(int fd, short what, void *arg)
{
  struct airplay_buffered_stream *bs = arg;
  uint8_t buf[512];
  char hex[64 * 3 + 1];
  int ret;
  int i;
  int n;

  ret = recv(fd, buf, sizeof(buf), 0);
  if (ret > 0)
    {
      n = (ret < 64) ? ret : 64;
      hex[0] = '\0';
      for (i = 0; i < n; i++)
	snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02x ", buf[i]);

      DPRINTF(E_LOG, L_AIRPLAY, "Buffered socket INBOUND from receiver: %d bytes: %s\n", ret, hex);
      return;
    }

  // EOF and hard errors must disarm the event. EV_READ is level-triggered, so
  // a closed or errored socket stays permanently readable - a persistent event
  // left armed would re-enter this callback forever, spinning the event loop
  // and flooding the log. Disarming only stops the monitoring; we deliberately
  // do not fail the session, since this monitoring must not change behaviour
  // (the write path decides the session's fate, as it did before).
  if (ret == 0)
    DPRINTF(E_LOG, L_AIRPLAY, "Buffered socket closed by receiver, read monitoring stopped\n");
  else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
    return; // Nothing to read after all - normal on a nonblocking socket
  else
    DPRINTF(E_LOG, L_AIRPLAY, "Buffered socket read error: %s, read monitoring stopped\n", strerror(errno));

  event_del(bs->readev);
}


/* ------------------------------- Interface ------------------------------- */

int
airplay_buffered_start(struct airplay_buffered_stream **bs, struct event_base *evbase, const char *address, unsigned short data_port, const uint8_t *shk, size_t shk_len, uint32_t format_id)
{
  struct airplay_buffered_stream *stream;
  int fd;
  int flags;

  if (shk_len < AIRPLAY_BUFFERED_KEY_LEN)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "AirPlay buffered audio shared key too short (%zu bytes)\n", shk_len);
      return -1;
    }

  fd = net_connect(address, data_port, SOCK_STREAM, "AirPlay data");
  if (fd < 0)
    return -1;

  // net_connect() leaves the socket blocking; the write path below must
  // never block the caller, so switch to nonblocking after connect (same
  // approach as the poll-based connect in net_connect_addrinfo() itself)
  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not set AirPlay buffered audio socket nonblocking: %s\n", strerror(errno));
      close(fd);
      return -1;
    }

  CHECK_NULL(L_AIRPLAY, stream = calloc(1, sizeof(struct airplay_buffered_stream)));

  stream->fd = fd;
  stream->format_id = format_id;

  stream->cipher_hd = chacha_open(shk, AIRPLAY_BUFFERED_KEY_LEN);
  if (!stream->cipher_hd)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not set up AirPlay buffered audio cipher\n");
      goto error;
    }

  CHECK_NULL(L_AIRPLAY, stream->outbuf = evbuffer_new());

  stream->writeev = event_new(evbase, fd, EV_WRITE, write_cb, stream);
  if (!stream->writeev)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not create AirPlay buffered audio write event\n");
      goto error;
    }

  // Persistent read event, always armed, so any unsolicited data the
  // receiver writes back on this socket is logged rather than left to pile
  // up unread (see read_cb() above)
  stream->readev = event_new(evbase, fd, EV_READ | EV_PERSIST, read_cb, stream);
  if (!stream->readev)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not create AirPlay buffered audio read event\n");
      goto error;
    }

  event_add(stream->readev, NULL);
  DPRINTF(E_LOG, L_AIRPLAY, "Buffered socket read monitoring armed (fd %d)\n", fd);

  *bs = stream;
  return 0;

 error:
  if (stream->readev)
    event_free(stream->readev);
  if (stream->writeev)
    event_free(stream->writeev);
  if (stream->outbuf)
    evbuffer_free(stream->outbuf);
  chacha_close(stream->cipher_hd);
  free(stream);
  close(fd);

  return -1;
}

int
airplay_buffered_write(struct airplay_buffered_stream *bs, const uint8_t *frame, size_t len, uint32_t seqnum, uint32_t rtptime, bool marker)
{
  int ret;

  if (!bs || bs->failed)
    return -1;

  ret = frame_encrypt_and_queue(bs, frame, len, seqnum, rtptime, marker);
  if (ret < 0)
    {
      bs->failed = true;
      return -1;
    }

  event_add(bs->writeev, NULL);

  return 0;
}

size_t
airplay_buffered_pending(struct airplay_buffered_stream *bs)
{
  if (!bs)
    return 0;

  return evbuffer_get_length(bs->outbuf);
}

void
airplay_buffered_stop(struct airplay_buffered_stream *bs)
{
  if (!bs)
    return;

  // Free the event before closing the fd it references to avoid a
  // use-after-free/UB window if the event loop is still iterating
  if (bs->writeev)
    event_free(bs->writeev);
  if (bs->readev)
    event_free(bs->readev);

  if (bs->outbuf)
    evbuffer_free(bs->outbuf);

  chacha_close(bs->cipher_hd);

  if (bs->fd >= 0)
    close(bs->fd);

  free(bs);
}
