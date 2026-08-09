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
#include <time.h>
#include <inttypes.h>
#include <unistd.h>

#include <event2/buffer.h>
#include <plist/plist.h>

#include "misc.h"
#include "logger.h"
#include "plist_wrap.h"
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

/* ---------------------------- message builders ----------------------------- */

// ProtocolMessage field 85 and DeviceInfoMessage field 1 are both
// "uniqueIdentifier" strings shaped like a UUID (8-4-4-4-12 uppercase hex).
// Observed receiver behaviour indicates it only cares about the
// shape, not any particular derivation algorithm - the reference mints
// field 85 fresh per message and derives field 1 from the sender's
// identity, but nothing in the confirmed field table implies the receiver
// validates either value beyond "looks like a UUID". This derives a
// UUID-shaped string from a 64-bit seed by hashing it twice with different
// seeds into 16 bytes and patching in version-5/variant bits - deterministic
// and collision-resistant enough for a per-session/per-message identifier,
// without pulling in a UUID library dependency.
static void
mrp_uuid_from_seed(char out[37], uint64_t seed)
{
  uint64_t hi = murmur_hash64(&seed, sizeof(seed), 0);
  uint64_t lo = murmur_hash64(&seed, sizeof(seed), 1);
  uint8_t b[16];
  int i;

  for (i = 0; i < 8; i++)
    b[i] = (uint8_t)(hi >> (8 * (7 - i)));
  for (i = 0; i < 8; i++)
    b[8 + i] = (uint8_t)(lo >> (8 * (7 - i)));

  b[6] = (b[6] & 0x0f) | 0x50; // version 5
  b[8] = (b[8] & 0x3f) | 0x80; // RFC 4122 variant

  snprintf(out, 37, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
      b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
      b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

// Builds the DEVICE_INFO ProtocolMessage (proto2: field 1 = type 15, field
// 20 = the DeviceInfoMessage submessage, field 85 = a UUID string) and wraps
// it as { params: { data: <varint length prefix + protobuf> } } - no outer
// "type" key. devname is the SENDER's own advertised name (not the
// receiver's); dacp_id is the exact DACP-ID header string airplay.c already
// sends on every request. session_uuid/group_uuid (fields 41/42) are omitted
// from the submessage when empty, matching a session that hasn't joined a
// group yet.
int
airplay_mrp_deviceinfo_make(struct evbuffer *evbuf, struct airplay_mrp *mrp, const char *devname, const char *session_uuid, const char *group_uuid, const char *dacp_id)
{
  struct evbuffer *submsg;
  struct evbuffer *protomsg;
  struct evbuffer *blob;
  plist_t root;
  plist_t params;
  uint8_t *proto_data;
  size_t proto_len;
  uint8_t *plist_data;
  size_t plist_len;
  char device_uuid[37];
  char msg_uuid[37];
  uint64_t dacp_hash;
  int ret;

  CHECK_NULL(L_AIRPLAY, submsg = evbuffer_new());
  CHECK_NULL(L_AIRPLAY, protomsg = evbuffer_new());

  // Field 1 (DeviceInfoMessage.uniqueIdentifier): deterministic from the
  // sender's own identity so it is stable across messages/sessions - the
  // receiver only appears to care about the UUID shape, so hashing the
  // DACP-ID (the sender's stable per-instance identity string) is
  // sufficient; there is no requirement it match any particular derivation.
  dacp_hash = dacp_id ? murmur_hash64(dacp_id, (int)strlen(dacp_id), 0) : 0;
  mrp_uuid_from_seed(device_uuid, dacp_hash);

  mrp_pb_add_string_field(submsg, 1, device_uuid);
  mrp_pb_add_string_field(submsg, 2, devname ? devname : "");
  mrp_pb_add_string_field(submsg, 3, "iPhone");
  mrp_pb_add_string_field(submsg, 4, "21F90");
  mrp_pb_add_string_field(submsg, 5, "com.apple.Music");
  mrp_pb_add_varint_field(submsg, 7, 1);
  mrp_pb_add_varint_field(submsg, 8, 139);
  mrp_pb_add_bool_field(submsg, 9, true);
  mrp_pb_add_bool_field(submsg, 10, true);
  mrp_pb_add_string_field(submsg, 12, "com.apple.Music");
  mrp_pb_add_bool_field(submsg, 13, true);
  mrp_pb_add_bool_field(submsg, 14, true);
  mrp_pb_add_bool_field(submsg, 15, true);
  mrp_pb_add_varint_field(submsg, 17, 2);
  mrp_pb_add_string_field(submsg, 19, dacp_id ? dacp_id : "");
  mrp_pb_add_varint_field(submsg, 21, 1);
  mrp_pb_add_varint_field(submsg, 22, 1);
  mrp_pb_add_string_field(submsg, 31, "com.apple.podcasts");
  mrp_pb_add_string_field(submsg, 39, "iPhone17,1");
  if (session_uuid && session_uuid[0])
    mrp_pb_add_string_field(submsg, 41, session_uuid);
  if (group_uuid && group_uuid[0])
    mrp_pb_add_string_field(submsg, 42, group_uuid);
  mrp_pb_add_string_field(submsg, 43, "com.apple.iBooks");

  // ProtocolMessage: field 1 (type) first, then field 20 (the submessage
  // just built), then field 85 - a UUID string. A per-session derivation
  // (from the session's own uuid where available, else the device_uuid just
  // derived) is acceptable, since field 85's only
  // observed requirement is "looks like a UUID"; this keeps the value
  // deterministic per session rather than pulling in a random source.
  mrp_pb_add_varint_field(protomsg, 1, 15);
  mrp_pb_add_bytes_field(protomsg, 20, evbuffer_pullup(submsg, -1), evbuffer_get_length(submsg));
  mrp_uuid_from_seed(msg_uuid, murmur_hash64(session_uuid && session_uuid[0] ? session_uuid : device_uuid,
      (int)strlen(session_uuid && session_uuid[0] ? session_uuid : device_uuid), 1));
  mrp_pb_add_string_field(protomsg, 85, msg_uuid);

  evbuffer_free(submsg);

  proto_len = evbuffer_get_length(protomsg);
  proto_data = evbuffer_pullup(protomsg, -1);

  CHECK_NULL(L_AIRPLAY, blob = evbuffer_new());
  mrp_pb_add_varint(blob, proto_len);
  evbuffer_add(blob, proto_data, proto_len);

  params = plist_new_dict();
  wplist_dict_add_data(params, "data", evbuffer_pullup(blob, -1), evbuffer_get_length(blob));
  evbuffer_free(blob);
  evbuffer_free(protomsg);

  root = plist_new_dict();
  plist_dict_set_item(root, "params", params);

  ret = wplist_to_bin(&plist_data, &plist_len, root);
  plist_free(root);
  if (ret < 0)
    return -1;

  evbuffer_add(evbuf, plist_data, plist_len);
  free(plist_data);

  return 0;
}

// Derives the 16-lowercase-hex-char ArtworkIdentifier from the artwork
// content itself (a 64-bit hash of the bytes), rather than a random token:
// this gives us "same bytes -> same identifier" for free, which is what the
// receiver needs to avoid a spurious re-render, without having to keep
// the actual bytes around
// just to compare them next time.
static void
mrp_artwork_id_make(char out[17], const uint8_t *artwork, size_t artwork_len)
{
  uint64_t hash = murmur_hash64(artwork, (int)artwork_len, 0);

  snprintf(out, 17, "%016" PRIx64, hash);
}

// Derives a per-track UniqueIdentifier: a 64-bit hash over the fields that
// identify "this is the same track" (item id plus the tag triple, so a
// same-item_id refinement of the tags still lands on a related but distinct
// value - the id only needs to be stable across refinements, which is
// enforced by the caller not re-deriving it, not by the hash itself), masked
// to 63 bits since kMRMediaRemoteNowPlayingInfoUniqueIdentifier must be a
// non-negative int64.
static uint64_t
mrp_uid_make(uint32_t item_id, const char *title, const char *artist, const char *album)
{
  char buf[512];
  int len;

  len = snprintf(buf, sizeof(buf), "%" PRIu32 "|%s|%s|%s",
      item_id, title ? title : "", artist ? artist : "", album ? album : "");
  if (len < 0)
    len = 0;
  else if ((size_t)len >= sizeof(buf))
    len = sizeof(buf) - 1;

  return murmur_hash64(buf, len, 0) & 0x7fffffffffffffffULL;
}

int
airplay_mrp_nowplaying_make(struct evbuffer *evbuf, struct airplay_mrp *mrp, uint32_t item_id,
    const char *title, const char *artist, const char *album,
    double duration_s, double elapsed_s, bool playing,
    const uint8_t *artwork, size_t artwork_len, const char *artwork_mime, bool timeline_only)
{
  plist_t root;
  plist_t outer_params;
  plist_t inner_params;
  struct timespec now;
  double now_s;
  uint8_t *data;
  size_t len;
  int ret;

  clock_gettime(CLOCK_REALTIME, &now);
  now_s = (double)now.tv_sec + (double)now.tv_nsec / 1e9;

  inner_params = plist_new_dict();

  if (!timeline_only)
    {
      if (item_id != mrp->last_item_id)
	{
	  mrp->nowplaying_uid = mrp_uid_make(item_id, title, artist, album);
	  mrp->last_item_id = item_id;
	}

      wplist_dict_add_string(inner_params, "kMRMediaRemoteNowPlayingInfoTitle", title ? title : "");
      wplist_dict_add_string(inner_params, "kMRMediaRemoteNowPlayingInfoArtist", artist ? artist : "");
      wplist_dict_add_string(inner_params, "kMRMediaRemoteNowPlayingInfoAlbum", album ? album : "");
      if (duration_s > 0.0)
	wplist_dict_add_real(inner_params, "kMRMediaRemoteNowPlayingInfoDuration", duration_s);
    }

  wplist_dict_add_real(inner_params, "kMRMediaRemoteNowPlayingInfoElapsedTime", elapsed_s);
  wplist_dict_add_real(inner_params, "kMRMediaRemoteNowPlayingInfoPlaybackRate", playing ? 1.0 : 0.0);
  wplist_dict_add_real(inner_params, "kMRMediaRemoteNowPlayingInfoDefaultPlaybackRate", 1.0);
  wplist_dict_add_date(inner_params, "kMRMediaRemoteNowPlayingInfoTimestamp", now_s);

  if (!timeline_only)
    {
      wplist_dict_add_string(inner_params, "kMRMediaRemoteNowPlayingInfoMediaType", "MRMediaRemoteMediaTypeMusic");
      wplist_dict_add_int(inner_params, "kMRMediaRemoteNowPlayingInfoUniqueIdentifier", (int64_t)mrp->nowplaying_uid);

      if (artwork && artwork_len > 0)
	{
	  // Content-derived, so identical bytes always land on the same
	  // identifier without needing to keep the previous bytes around to
	  // compare - and a real content change naturally mints a new one.
	  // Keeping the identifier stable across unchanged bytes matters: an
	  // identifier flip alone triggers a visible receiver-side re-render.
	  mrp_artwork_id_make(mrp->artwork_id, artwork, artwork_len);

	  wplist_dict_add_string(inner_params, "kMRMediaRemoteNowPlayingInfoArtworkIdentifier", mrp->artwork_id);
	  wplist_dict_add_string(inner_params, "kMRMediaRemoteNowPlayingInfoArtworkMIMEType", artwork_mime ? artwork_mime : "image/jpeg");
	  wplist_dict_add_data(inner_params, "kMRMediaRemoteNowPlayingInfoArtworkData", (uint8_t *)artwork, artwork_len);
	}
      else
	{
	  mrp->artwork_id[0] = '\0';
	}
    }

  outer_params = plist_new_dict();
  wplist_dict_add_string(outer_params, "type", "npi-text");
  wplist_dict_add_string(outer_params, "mergePolicy", timeline_only ? "update" : "replace");
  plist_dict_set_item(outer_params, "params", inner_params);

  root = plist_new_dict();
  wplist_dict_add_string(root, "type", "updateMRNowPlayingInfo");
  plist_dict_set_item(root, "params", outer_params);

  ret = wplist_to_bin(&data, &len, root);
  plist_free(root);
  if (ret < 0)
    return -1;

  evbuffer_add(evbuf, data, len);
  free(data);

  return 0;
}

int
airplay_mrp_playback_state_make(struct evbuffer *evbuf, struct airplay_mrp *mrp, int state)
{
  plist_t root;
  plist_t params;
  uint8_t *data;
  size_t len;
  int ret;

  params = plist_new_dict();
  wplist_dict_add_uint(params, "mrPlaybackState", (uint64_t)state);

  root = plist_new_dict();
  wplist_dict_add_string(root, "type", "updateMRPlaybackState");
  plist_dict_set_item(root, "params", params);

  ret = wplist_to_bin(&data, &len, root);
  plist_free(root);
  if (ret < 0)
    return -1;

  evbuffer_add(evbuf, data, len);
  free(data);

  mrp->last_playback_state = state;

  return 0;
}

// Builds the bare (unprefixed, no ProtocolMessage envelope) NowPlayingClient
// protobuf - f1 pid, f2 bundle id, f7 display name - and wraps it as
// { type: "updateMRNowPlayingClient", params: { mrNowPlayingClient: <data> } },
// matching what a real Apple sender emits.
int
airplay_mrp_nowplaying_client_make(struct evbuffer *evbuf, struct airplay_mrp *mrp, const char *devname)
{
  struct evbuffer *proto;
  plist_t root;
  plist_t params;
  uint8_t *proto_data;
  size_t proto_len;
  uint8_t *plist_data;
  size_t plist_len;
  int ret;

  CHECK_NULL(L_AIRPLAY, proto = evbuffer_new());

  mrp_pb_add_varint_field(proto, 1, (uint64_t)getpid());
  mrp_pb_add_string_field(proto, 2, "com.apple.Music");
  mrp_pb_add_string_field(proto, 7, devname ? devname : "");

  proto_len = evbuffer_get_length(proto);
  proto_data = evbuffer_pullup(proto, -1);

  params = plist_new_dict();
  wplist_dict_add_data(params, "mrNowPlayingClient", proto_data, proto_len);
  evbuffer_free(proto);

  root = plist_new_dict();
  wplist_dict_add_string(root, "type", "updateMRNowPlayingClient");
  plist_dict_set_item(root, "params", params);

  ret = wplist_to_bin(&plist_data, &plist_len, root);
  plist_free(root);
  if (ret < 0)
    return -1;

  evbuffer_add(evbuf, plist_data, plist_len);
  free(plist_data);

  mrp->registered = true;

  return 0;
}
