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

#ifndef __AIRPLAY_MRP_H__
#define __AIRPLAY_MRP_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct evbuffer;

// Per-session MediaRemote (MRP) state, hung off struct airplay_session once
// a receiver advertises MetadataFeatures_3 (bit 50) and the config allows
// it. Nothing allocates or consumes this yet - lifecycle only for now.
struct airplay_mrp
{
  // Current track's kMRMediaRemoteNowPlayingInfoUniqueIdentifier. Re-minted
  // on a track change, held stable across same-track tag refinements.
  uint64_t nowplaying_uid;

  // Current kMRMediaRemoteNowPlayingInfoArtworkIdentifier (16 lowercase hex
  // chars + nul). Held stable while the artwork bytes are unchanged, since a
  // receiver-visible re-render is triggered by an identifier flip alone.
  char artwork_id[17];

  // Queue item id backing the current nowplaying_uid/artwork_id, used to
  // detect a track change vs. a same-track metadata refinement.
  uint32_t last_item_id;

  // Last mrPlaybackState value actually sent (1 Playing / 2 Paused /
  // 3 Stopped), for dedup'ing updateMRPlaybackState pushes.
  int last_playback_state;

  // Set once DEVICE_INFO has been sent - it must precede every other MRP
  // message on the session, and is only sent once.
  bool device_info_sent;

  // Set once the "extended registration" burst (supported commands /
  // playback state / nowplaying client) has run - it runs once, with the
  // first now-playing push of the session.
  bool registered;
};

// Allocates and zero-initialises a new per-session MRP state blob. Returns
// NULL on allocation failure.
struct airplay_mrp *
airplay_mrp_new(void);

// Frees a per-session MRP state blob. Accepts NULL.
void
airplay_mrp_free(struct airplay_mrp *mrp);

// Message-builder stubs. Each will, once implemented, append the bplist
// (and where applicable, proto2-encoded) body for its message to evbuf and
// return 0 on success, -1 on error. Not wired into the send path yet - all
// currently return -1 unconditionally.

int
airplay_mrp_deviceinfo_make(struct evbuffer *evbuf, struct airplay_mrp *mrp, const char *devname, const char *session_uuid, const char *group_uuid, const char *dacp_id);

int
airplay_mrp_nowplaying_make(struct evbuffer *evbuf, struct airplay_mrp *mrp, const char *title, const char *artist, const char *album, double duration_s, double elapsed_s, bool playing, const uint8_t *artwork, size_t artwork_len);

int
airplay_mrp_playback_state_make(struct evbuffer *evbuf, struct airplay_mrp *mrp, int state);

int
airplay_mrp_nowplaying_client_make(struct evbuffer *evbuf, struct airplay_mrp *mrp, const char *devname);

#endif  /* !__AIRPLAY_MRP_H__ */
