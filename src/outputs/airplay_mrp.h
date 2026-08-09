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

// updateMRPlaybackState's mrPlaybackState values, sent directly as the
// plist integer.
#define AIRPLAY_MRP_STATE_PLAYING  1
#define AIRPLAY_MRP_STATE_PAUSED   2
#define AIRPLAY_MRP_STATE_STOPPED  3

// Per-session MediaRemote (MRP) state, hung off struct airplay_session once
// a receiver advertises MetadataFeatures_3 (bit 50) and the config allows
// it.
struct airplay_mrp
{
  // Current track's kMRMediaRemoteNowPlayingInfoUniqueIdentifier.
  // Content-derived (hash of item id + title/artist/album) and recomputed
  // on every replace push: the receiver keys its now-playing item on this
  // value, so it must change exactly when the track content does - the
  // pipe queue's single item id never changes, so the tag triple is what
  // distinguishes tracks.
  uint64_t nowplaying_uid;

  // Current kMRMediaRemoteNowPlayingInfoArtworkIdentifier (16 lowercase hex
  // chars + nul). Held stable while the artwork bytes are unchanged, since a
  // receiver-visible re-render is triggered by an identifier flip alone.
  char artwork_id[17];

  // Last mrPlaybackState value actually sent (AIRPLAY_MRP_STATE_*), for
  // dedup'ing updateMRPlaybackState pushes. Starts at 0, which is not a
  // valid state, so the first push of a session is always sent regardless
  // of what "playing" state a caller computes.
  int last_playback_state;

  // Session-level pause/resume intent, set by the airplay.c flush/resume
  // hooks. Drives both PlaybackRate inside NowPlayingInfo bodies and the
  // mrPlaybackState pushed alongside them (Paused when true, Playing when
  // false - Stopped is only ever used at teardown, out of scope for the
  // pause/resume hooks).
  bool paused;

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

// Message builders. Each appends the bplist (and where applicable,
// proto2-encoded) body for its message to evbuf and returns 0 on success,
// -1 on error. DEVICE_INFO and the NowPlayingClient builders are still
// stubs (commit 5 scope) - both currently return -1 unconditionally.

int
airplay_mrp_deviceinfo_make(struct evbuffer *evbuf, struct airplay_mrp *mrp, const char *devname, const char *session_uuid, const char *group_uuid, const char *dacp_id);

// Builds an updateMRNowPlayingInfo body.
//
// timeline_only selects between the two accepted message shapes:
//  - false ("replace"): the full key set - title/artist/album (always, empty
//    string if NULL), duration_s (omitted when <= 0), elapsed_s, playing,
//    MediaType, UniqueIdentifier (content-derived from item_id + the tag
//    triple, so it flips exactly on a real track change), and artwork (when
//    non-NULL) on every call. item_id, title, artist, album, duration_s and
//    artwork/artwork_len/artwork_mime are used.
//  - true ("update"): only ElapsedTime/PlaybackRate/DefaultPlaybackRate/
//    Timestamp. item_id, title, artist, album, duration_s and artwork are
//    ignored (pass 0/NULL) and mrp's track-identity state is left alone.
//
// artwork_mime is the MIME type to advertise for a non-NULL artwork (e.g.
// "image/jpeg"/"image/png" - the caller already has this mapping for the
// legacy SET_PARAMETER artwork send, see payload_make_send_artwork()).
//
// elapsed_s and playing apply in both shapes. Sets mrp->nowplaying_uid and
// mrp->artwork_id as a side effect of a "replace" call.
int
airplay_mrp_nowplaying_make(struct evbuffer *evbuf, struct airplay_mrp *mrp, uint32_t item_id,
    const char *title, const char *artist, const char *album,
    double duration_s, double elapsed_s, bool playing,
    const uint8_t *artwork, size_t artwork_len, const char *artwork_mime, bool timeline_only);

// Builds an updateMRPlaybackState body ({mrPlaybackState: state}, one of the
// AIRPLAY_MRP_STATE_* values). Unconditional - the caller decides whether a
// push is warranted (dedup, forced first send, etc). Updates
// mrp->last_playback_state as a side effect of a successful build.
int
airplay_mrp_playback_state_make(struct evbuffer *evbuf, struct airplay_mrp *mrp, int state);

int
airplay_mrp_nowplaying_client_make(struct evbuffer *evbuf, struct airplay_mrp *mrp, const char *devname);

#endif  /* !__AIRPLAY_MRP_H__ */
