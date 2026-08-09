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

/*
 * MediaRemote (MRP) support: session state and a minimal proto2 wire-format
 * emitter for the handful of protobuf-carrying /command messages
 * (DEVICE_INFO, updateMRNowPlayingClient). owntone has no protobuf library
 * anywhere else in the tree, so this is a small hand-rolled varint/
 * length-delimited encoder, not a generated stub - just enough proto2
 * emission to hand-build the fixed field sets these messages need.
 *
 * The plist envelopes the protobuf bodies ride in (bplist "type"/"params"
 * dicts) are built separately in airplay.c, using the existing plist_wrap.h
 * helpers, the same way payload_make_command_supported_commands() already
 * does.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <event2/buffer.h>

#include "misc.h"
#include "logger.h"
#include "airplay_mrp.h"

/* ------------------------------ proto2 emitter ----------------------------
 *
 * Hand-rolled proto2 wire-format helpers. No message descriptors, no
 * decoding - just enough to append the field set a caller already knows it
 * wants, in field-number order, into a caller-supplied growable evbuffer.
 */

// Wire types (proto2 wire format, google.protobuf).
#define MRP_PB_WIRETYPE_VARINT  0
#define MRP_PB_WIRETYPE_LENDELIM 2

// Appends val as a base-128 unsigned varint (LSB group first, continuation
// bit 0x80 set on every byte but the last). Handles the full 64-bit range.
__attribute__((unused)) static void
mrp_pb_add_varint(struct evbuffer *evbuf, uint64_t val)
{
  uint8_t byte;

  do
    {
      byte = val & 0x7f;
      val >>= 7;
      if (val)
	byte |= 0x80;

      evbuffer_add(evbuf, &byte, 1);
    }
  while (val);
}

// Appends a field tag: (field_no << 3) | wire_type, itself varint-encoded.
__attribute__((unused)) static void
mrp_pb_add_tag(struct evbuffer *evbuf, uint32_t field_no, int wire_type)
{
  mrp_pb_add_varint(evbuf, ((uint64_t)field_no << 3) | (wire_type & 0x7));
}

// Appends a complete varint-wire-type field: tag then value.
__attribute__((unused)) static void
mrp_pb_add_varint_field(struct evbuffer *evbuf, uint32_t field_no, uint64_t val)
{
  mrp_pb_add_tag(evbuf, field_no, MRP_PB_WIRETYPE_VARINT);
  mrp_pb_add_varint(evbuf, val);
}

// Appends a proto2 bool field (encoded as a 0/1 varint).
__attribute__((unused)) static void
mrp_pb_add_bool_field(struct evbuffer *evbuf, uint32_t field_no, bool val)
{
  mrp_pb_add_varint_field(evbuf, field_no, val ? 1 : 0);
}

// Appends a length-delimited field: tag, then a varint byte length, then the
// raw bytes. Used for both string and embedded-message (submessage) fields -
// proto2 encodes both identically on the wire.
__attribute__((unused)) static void
mrp_pb_add_bytes_field(struct evbuffer *evbuf, uint32_t field_no, const uint8_t *data, size_t len)
{
  mrp_pb_add_tag(evbuf, field_no, MRP_PB_WIRETYPE_LENDELIM);
  mrp_pb_add_varint(evbuf, len);
  if (len > 0)
    evbuffer_add(evbuf, data, len);
}

// Appends a length-delimited string field (nul-terminated C string, nul not
// included in the wire length, per proto2 string semantics).
__attribute__((unused)) static void
mrp_pb_add_string_field(struct evbuffer *evbuf, uint32_t field_no, const char *str)
{
  mrp_pb_add_bytes_field(evbuf, field_no, (const uint8_t *)str, str ? strlen(str) : 0);
}

/* -------------------------------- lifecycle -------------------------------- */

struct airplay_mrp *
airplay_mrp_new(void)
{
  struct airplay_mrp *mrp;

  CHECK_NULL(L_AIRPLAY, mrp = calloc(1, sizeof(struct airplay_mrp)));

  return mrp;
}

void
airplay_mrp_free(struct airplay_mrp *mrp)
{
  free(mrp);
}

/* ---------------------------- message builders -----------------------------
 *
 * Stubs only - the field sets and outer bplist envelopes are added in a
 * later commit. Not wired into any send path yet.
 */

int
airplay_mrp_deviceinfo_make(struct evbuffer *evbuf, struct airplay_mrp *mrp, const char *devname, const char *session_uuid, const char *group_uuid, const char *dacp_id)
{
  return -1;
}

int
airplay_mrp_nowplaying_make(struct evbuffer *evbuf, struct airplay_mrp *mrp, const char *title, const char *artist, const char *album, double duration_s, double elapsed_s, bool playing, const uint8_t *artwork, size_t artwork_len)
{
  return -1;
}

int
airplay_mrp_playback_state_make(struct evbuffer *evbuf, struct airplay_mrp *mrp, int state)
{
  return -1;
}

int
airplay_mrp_nowplaying_client_make(struct evbuffer *evbuf, struct airplay_mrp *mrp, const char *devname)
{
  return -1;
}
