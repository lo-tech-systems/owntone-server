/*
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
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <time.h>
#include <strings.h>
#include <sys/utsname.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <gcrypt.h>

#include "plist_wrap.h"

#include "evrtsp/evrtsp.h"
#include "conffile.h"
#include "logger.h"
#include "mdns.h"
#include "misc.h"
#include "player.h"
#include "db.h"
#include "artwork.h"
#include "dmap_common.h"
#include "rtp_common.h"
#include "transcode.h"
#include "ptpd.h"
#include "outputs.h"

#include "airplay_events.h"
#include "airplay_buffered.h"
#include "airplay_common.h"
#include "airplay_encoder.h"
#include "pair_ap/pair.h"

/* List of TODO's for AirPlay 2
 *
 * inplace encryption
 * latency needs different handling
 * support ipv6, e.g. in SETPEERS
 *
 */

// Airplay 2 has a gazallion parameters, many of them unknown to us. With the
// below it is possible to easily try different variations.
#define AIRPLAY_USE_STREAMID                 0
#define AIRPLAY_USE_PAIRING_TRANSIENT        1
#define AIRPLAY_USE_AUTH_SETUP               0

// Full traffic dumps in the log in debug mode
#define AIRPLAY_DUMP_TRAFFIC                 0

#define AIRPLAY_QUALITY_SAMPLE_RATE_DEFAULT     44100
#define AIRPLAY_QUALITY_BITS_PER_SAMPLE_DEFAULT 16
#define AIRPLAY_QUALITY_CHANNELS_DEFAULT        2

// AirTunes v2 number of samples per packet
// Probably using this value because 44100/352 and 48000/352 has good 32 byte
// alignment, which improves performance of some encoders
#define AIRPLAY_SAMPLES_PER_PACKET              352

// 0x60 Real Time Audio, 0x67 Buffered
#define AIRPLAY_RTP_PAYLOADTYPE                 0x60

// Quoting shairport-sync's rtp.c:
// The value of 11025 (0.25 seconds) is a guess based on the "Audio-Latency"
// parameter returned by an AE. Sigh, it would be nice to have a published
// protocol...
#define AIRPLAY_AUDIO_LATENCY_MS                250

// For transient pairing the key_len will be 64 bytes, but only 32 are used for
// audio payload encryption. For normal pairing the key is 32 bytes.
#define AIRPLAY_AUDIO_KEY_LEN 32

// How many RTP packets to buffer for retransmission
#define AIRPLAY_PACKET_BUFFER_SIZE    1000

// Buffered AirPlay 2 (type 103) format ids. These are the bufferStream
// capability ids a receiver advertises in /info and the per-packet format
// selector negotiated in SETUP(stream); the AAC-LC stereo id is also the
// universal baseline every AirPlay 2 receiver is expected to accept over the
// buffered transport.
#define AIRPLAY_FORMAT_ID_AAC_STEREO        23
// ALAC 48kHz/24-bit stereo, for lossless sources. The bufferStream capability
// list encodes each format id as a bit position (format id N is bit N of the
// mask), the same convention SETUP(stream)'s audioFormat field uses.
#define AIRPLAY_FORMAT_ID_ALAC48K_24_STEREO 21
// AAC-LC 48kHz 5.1. Used for both surround buffered kinds (static pan and
// decode upmix) - they share a format id because the receiver only cares
// about the stream layout, not how the sender chose to fill the channels.
#define AIRPLAY_FORMAT_ID_AAC_51            39
#define AIRPLAY_BUFFERED_SPF                 1024
// Samples per frame for the ALAC 48k/24 profile. ALAC decoders are
// initialised from a magic cookie carrying frameLength=352 - the classic ALAC
// frame size - not the larger frame size a generic encoder would default to,
// which a receiver's ALAC decoder will not recognise. Must match
// settings->frame_size in transcode.c's XCODE_ALAC48K_24_STEREO.
#define AIRPLAY_BUFFERED_SPF_ALAC24          352
// RTP SSRC for buffered ALAC 48k/24. Receivers select their decoder for the
// buffered stream from the SSRC field rather than negotiation alone, so the
// codec has to be stamped into it. owntone otherwise leaves the PTP session's
// SSRC at 0, which receivers tolerate for AAC but not for ALAC.
#define AIRPLAY_SSRC_ID_ALAC48K_24          0x15000000
// Implied by the format ids (the buffered SETUP carries no explicit "sr");
// this is the timescale of the buffered rtptime and the anchor's rtpTime.
#define AIRPLAY_BUFFERED_SAMPLE_RATE         48000
// SETUP(stream) audioFormat is a 64-bit bitmask in which the bufferStream
// format ids are BIT POSITIONS (same convention as the receiver's advertised
// capability mask), so the field value is 1 << format id.
#define AIRPLAY_BUFFERED_AUDIOFORMAT(format_id) (UINT64_C(1) << (format_id))
// RTP "type" value for SETUP(stream) when using the buffered transport (see
// the AIRPLAY_RTP_PAYLOADTYPE comment above: 0x60 real time, 0x67 buffered)
#define AIRPLAY_BUFFERED_RTP_TYPE             0x67
// Some receivers reject a SETRATEANCHORTIME in-band (RTSP 200 with a nonzero
// plist status) when it is placed before their clock has locked onto our PTP
// timeline; the anchor is retried a bounded number of times before the
// session is failed outright. Audio queued in the meantime is simply held by
// the receiver, so streaming state is unaffected while a retry is pending.
#define AIRPLAY_BUFFERED_ANCHOR_RETRIES_MAX   4
#define AIRPLAY_BUFFERED_ANCHOR_RETRY_WAIT_MS 1500
// A session joining an already-anchored master session gets an anchor this
// many frames past the current write head, and delivery to it is held until
// the write head reaches that point - so its first delivered frame matches
// its anchor exactly. Must comfortably exceed the anchor request/response
// round trip; 48 frames is about 1s at 1024 spf / 48kHz.
#define AIRPLAY_BUFFERED_JOIN_SLACK_FRAMES    48

// Admission weights per buffered kind, in abstract cost units. The decode
// upmix (6ch surround filter + 6ch AAC) dominates measured CPU on Cortex-A53;
// the other kinds are a fraction of a core each.
#define ENCODER_COST_SURROUND_UPMIX 6
#define ENCODER_COST_DEFAULT        2   // AAC stereo, ALAC24, static pan

#define AIRPLAY_MD_DELAY_STARTUP      15360
#define AIRPLAY_MD_DELAY_SWITCH       (AIRPLAY_MD_DELAY_STARTUP * 2)
#define AIRPLAY_MD_WANTS_TEXT         (1 << 0)
#define AIRPLAY_MD_WANTS_ARTWORK      (1 << 1)
#define AIRPLAY_MD_WANTS_PROGRESS     (1 << 2)

// ATV4 and Homepod disconnect for reasons that are not clear, but sending them
// progress metadata at regular intervals reduces the problem. The below
// interval was determined via testing, see:
// https://github.com/owntone/owntone-server/issues/734#issuecomment-622959334
#define AIRPLAY_KEEP_ALIVE_INTERVAL   25

// Some devices (notably HomePod stereo pairs) briefly refuse new RTSP
// connections for a few seconds after a TEARDOWN. On a soft start failure
// (connection refused, GET /info negative, timeout) we retry up to
// AIRPLAY_START_RETRY_MAX times, AIRPLAY_START_RETRY_WAIT_SEC apart. Bounded
// and delayed so the start path can never spin (cf. the old GET /info loop).
#define AIRPLAY_START_RETRY_MAX       3
#define AIRPLAY_START_RETRY_WAIT_SEC  3

// This is an arbitrary value which just needs to be kept in sync with the config
#define AIRPLAY_CONFIG_MAX_VOLUME     11

/* Keep in sync with const char *airplay_devtype */
enum airplay_devtype {
  AIRPLAY_DEV_APEX2_80211N,
  AIRPLAY_DEV_APEX3_80211N,
  AIRPLAY_DEV_APPLETV,
  AIRPLAY_DEV_APPLETV4,
  AIRPLAY_DEV_HOMEPOD,
  AIRPLAY_DEV_OTHER,
};

// Session is starting up
#define AIRPLAY_STATE_F_STARTUP    (1 << 13)
// Streaming is up (connection established)
#define AIRPLAY_STATE_F_CONNECTED  (1 << 14)
// Couldn't start device
#define AIRPLAY_STATE_F_FAILED     (1 << 15)

enum airplay_state {
  // Device is stopped (no session)
  AIRPLAY_STATE_STOPPED   = 0,
  // Session startup
  AIRPLAY_STATE_INFO      = AIRPLAY_STATE_F_STARTUP | 0x01,
  AIRPLAY_STATE_ENCRYPTED = AIRPLAY_STATE_F_STARTUP | 0x02,
  AIRPLAY_STATE_SETUP     = AIRPLAY_STATE_F_STARTUP | 0x03,
  AIRPLAY_STATE_RECORD    = AIRPLAY_STATE_F_STARTUP | 0x04,
  // Session established
  // - streaming ready (RECORD sent and acked, connection established)
  // - commands (SET_PARAMETER) are possible
  AIRPLAY_STATE_CONNECTED = AIRPLAY_STATE_F_CONNECTED | 0x01,
  // Media data is being sent
  AIRPLAY_STATE_STREAMING = AIRPLAY_STATE_F_CONNECTED | 0x02,
  // Session teardown in progress (-> going to STOPPED state)
  AIRPLAY_STATE_TEARDOWN  = AIRPLAY_STATE_F_CONNECTED | 0x03,
  // Session is failed, couldn't startup or error occurred
  AIRPLAY_STATE_FAILED    = AIRPLAY_STATE_F_FAILED | 0x01,
  // Pending PIN or password
  AIRPLAY_STATE_AUTH      = AIRPLAY_STATE_F_FAILED | 0x02,
};

enum airplay_seq_type
{
  AIRPLAY_SEQ_ABORT = -1,
  AIRPLAY_SEQ_START,
  AIRPLAY_SEQ_START_PLAYBACK,
  AIRPLAY_SEQ_PROBE,
  AIRPLAY_SEQ_FLUSH,
  AIRPLAY_SEQ_STOP,
  AIRPLAY_SEQ_FAILURE,
  AIRPLAY_SEQ_PIN_START,
  AIRPLAY_SEQ_SEND_VOLUME,
  AIRPLAY_SEQ_SEND_TEXT,
  AIRPLAY_SEQ_SEND_PROGRESS,
  AIRPLAY_SEQ_SEND_ARTWORK,
  AIRPLAY_SEQ_PAIR_SETUP,
  AIRPLAY_SEQ_PAIR_VERIFY,
  AIRPLAY_SEQ_PAIR_TRANSIENT,
  AIRPLAY_SEQ_FEEDBACK,
  AIRPLAY_SEQ_SEND_ANCHOR,
  AIRPLAY_SEQ_FLUSH_BUFFERED,
  AIRPLAY_SEQ_CONTINUE, // Must be last element
};

// From https://openairplay.github.io/airplay-spec/status_flags.html
enum airplay_status_flags
{
  AIRPLAY_FLAG_PROBLEM_DETECTED               = (1 << 0),
  AIRPLAY_FLAG_NOT_CONFIGURED                 = (1 << 1),
  AIRPLAY_FLAG_AUDIO_CABLE_ATTACHED           = (1 << 2),
  AIRPLAY_FLAG_PIN_REQUIRED                   = (1 << 3),
  AIRPLAY_FLAG_SUPPORTS_FROM_CLOUD            = (1 << 6),
  AIRPLAY_FLAG_PASSWORD_REQUIRED              = (1 << 7),
  AIRPLAY_FLAG_ONE_TIME_PAIRING_REQUIRED      = (1 << 9),
  AIRPLAY_FLAG_SETUP_HK_ACCESS_CTRL           = (1 << 10),
  AIRPLAY_FLAG_SUPPORTS_RELAY                 = (1 << 11),
  AIRPLAY_FLAG_SILENT_PRIMARY                 = (1 << 12),
  AIRPLAY_FLAG_TIGHT_SYNC_IS_GRP_LEADER       = (1 << 13),
  AIRPLAY_FLAG_TIGHT_SYNC_BUDDY_NOT_REACHABLE = (1 << 14),
  AIRPLAY_FLAG_IS_APPLE_MUSIC_SUBSCRIBER      = (1 << 15),
  AIRPLAY_FLAG_CLOUD_LIBRARY_ON               = (1 << 16),
  AIRPLAY_FLAG_RECEIVER_IS_BUSY               = (1 << 17),
};

// Info about the device, which is not required by the player, only internally
// True when a device may be offered/accept a 5.1 surround output mode.
// Surround is limited to a standalone Apple TV: not a member of a HomePod
// stereo pair, and not the visible leader of a HomePod-behind-Apple-TV proxy
// group (those groups play through their HomePod members, not the ATV
// itself). Used both when selecting a session's buffered kind and when
// reporting supported_modes to the API.
static bool
airplay_device_allows_surround(struct output_device *device);

// bufferStream format id a given buffered kind streams. AIRPLAY_BUFFERED_KIND_NONE
// has no meaningful format id (session isn't buffered); callers only use this
// once wants_buffered_kind/buffered_mode has already excluded that case.
static uint32_t
buffered_format_id_for_kind(enum airplay_buffered_kind kind)
{
  switch (kind)
    {
      case AIRPLAY_BUFFERED_KIND_ALAC24:          return AIRPLAY_FORMAT_ID_ALAC48K_24_STEREO;
      case AIRPLAY_BUFFERED_KIND_SURROUND_STEREO:
      case AIRPLAY_BUFFERED_KIND_SURROUND_UPMIX:  return AIRPLAY_FORMAT_ID_AAC_51;
      case AIRPLAY_BUFFERED_KIND_AAC_STEREO:
      case AIRPLAY_BUFFERED_KIND_NONE:
      default:                                    return AIRPLAY_FORMAT_ID_AAC_STEREO;
    }
}

struct airplay_extra
{
  enum airplay_devtype devtype;

  char *mdns_name;
  char *gid;
  char *gpn;
  char *tsid;

  uint16_t wanted_metadata;
  bool supports_auth_setup;
  bool supports_encryption;
  bool use_ptp;
  bool gcgl;
  bool igl;
};

struct airplay_master_session
{
  struct evbuffer *input_buffer;
  uint32_t input_buffer_samples;

  // ALAC encoder and buffer for encoded data. Realtime ams only - a buffered
  // ams leaves these NULL, encoding runs on its dedicated thread instead (see
  // encoder below).
  struct encode_ctx *encode_ctx;
  struct evbuffer *encoded_buffer;

  // Non-NULL iff buffered_kind != AIRPLAY_BUFFERED_KIND_NONE. Owns the encode
  // context and PCM/frame queues for this ams; runs on its own thread.
  struct airplay_encoder *encoder;

  struct rtp_session *rtp_session;

  struct rtcp_timestamp cur_stamp;

  uint8_t *rawbuf;
  size_t rawbuf_size;
  uint32_t samples_per_packet;

  // Session characteristics
  struct media_quality quality;
  bool use_ptp;

  // Number of samples that we tell the output to buffer (this will mean that
  // the position that we send in the sync packages are offset by this amount
  // compared to the rtptimes of the corresponding RTP packages we are sending)
  uint32_t output_buffer_samples;

  // Buffered AirPlay 2 (type 103) transport. When buffered is set, encode_ctx
  // above is one of the buffered encode profiles (not realtime ALAC) and
  // packets are framed via buffered_seqnum/buffered_rtptime and handed to
  // airplay_buffered_write() instead of rtp_session/rtp_packet_next
  // (rtp_session is still created and used for input quality subscription and
  // metadata rtptime calculations). buffered_kind selects which profile.
  bool buffered;
  enum airplay_buffered_kind buffered_kind;
  // Samples per buffered frame for this ams's encoder: AIRPLAY_BUFFERED_SPF
  // for every profile except ALAC24, which uses AIRPLAY_BUFFERED_SPF_ALAC24.
  // Set once in master_session_make() and used everywhere buffered_rtptime/
  // buffered_seqnum advance by one frame.
  uint32_t buffered_spf;
  uint32_t buffered_seqnum;
  uint32_t buffered_rtptime;
  bool buffered_marker_pending;
  // rtptime of the first frame actually written since session start or the
  // last FLUSHBUFFERED - i.e. the start of the audio sitting in the
  // receiver's buffer. SETRATEANCHORTIME's rtpTime must point here (the
  // anchor names the start of the buffered audio, not the write head).
  uint32_t buffered_anchor_rtptime;
  // Shared rtptime->networkTime mapping. The first anchor computed for any
  // session on this ams stores its (rtpTime, networkTime) point; anchors for
  // sessions joining later are DERIVED from it (same line, rate=1), so every
  // receiver places a given rtptime at the same wall-clock instant. Invalidated
  // by FLUSHBUFFERED (playback resumes at a new wall time) and by a rejected
  // first anchor (the retry must pick a fresh future time).
  bool buffered_anchor_mapped;
  uint32_t buffered_anchor_map_rtptime;
  uint64_t buffered_anchor_map_secs;
  uint64_t buffered_anchor_map_frac;

  struct airplay_master_session *next;
};

struct airplay_session
{
  uint64_t device_id;
  int callback_id;

  // Number of soft start retries already performed for this start attempt.
  // Carried across session recreation so the backoff stays bounded (see
  // start_retry()).
  int start_retries;

  struct airplay_master_session *master_session;

  struct evrtsp_connection *ctrl;

  enum airplay_state state;

  enum airplay_seq_type next_seq;

  uint64_t statusflags;
  uint16_t wanted_metadata;
  bool req_has_auth;
  bool supports_auth_setup;
  bool supports_encryption;

  struct event *deferredev;

  int reqs_in_flight;
  int cseq;

  uint32_t session_id;
  // Names the buffered stream's data connection in SETUP(stream). Kept
  // separate from session_id because that one also forms session_url, and
  // because this field carries a full signed 64-bit value.
  int64_t stream_connection_id;
  char session_url[128];
  char session_uuid[37];

  char group_uuid[37];
  // Always false for a generated group_uuid; reserved for a future stereo-pair
  // group id override.
  bool group_contains_leader;

  // Set when a request failed for a reason that is definitely NOT
  // authentication (the device replied, we could not use the reply). Stops
  // start_failure() from discarding perfectly good pairing keys.
  bool protocol_error;

  // Set once a response has been received over the encrypted control
  // channel, meaning the pairing keys have been proven for this connection.
  bool encrypted_ok;

  // Set for a capability probe (GET /info only, session torn down right
  // after). Distinguishes a probe from a real activation in the shared
  // /info handler, which must not run buffered admission/ams selection for
  // a session that is about to disappear anyway.
  bool probing;

  char *realm;
  char *nonce;
  const char *password;

  char *devname;
  char *address;
  int family;

  union net_sockaddr naddr;

  int volume;

  // device->offset_ms in samples (user config for correction of static
  // amplifier or DSP delays)
  int offset_samples;

  char *local_v4_address;
  char *local_v6_address;
  char local_mac_address[18];

  unsigned short data_port;
  unsigned short control_port;
  unsigned short events_port;

  // Buffered AirPlay 2 (type 103) transport. wants_buffered_kind is the
  // provisional decision, derived in session_make() from the device's
  // selected output mode (and the global buffered_audio_enabled switch under
  // "auto", and the standalone-ATV gate for the two surround modes);
  // device_supports_buffered reflects the /info capability gate
  // (response_handler_info_generic); buffered_mode is the final decision, also
  // made there, which is what everything downstream (SETUP payload, write
  // path, flush) acts on.
  enum airplay_buffered_kind wants_buffered_kind;
  // bufferStream format id this session will stream when buffered: 23 (AAC-LC
  // stereo), 21 (ALAC 48k/24 stereo) or 39 (5.1, either surround kind) -
  // derived from wants_buffered_kind, see buffered_format_id_for_kind().
  uint32_t buffered_format_id;
  bool device_supports_buffered;
  bool buffered_mode;
  // Set whenever a fresh SETRATEANCHORTIME is needed (buffered_mode just
  // decided, or after a FLUSHBUFFERED, which invalidates the receiver's
  // previous anchor)
  bool anchor_pending;
  // Set once the receiver accepts SETRATEANCHORTIME. No audio is written to
  // the data connection before this: it keeps the anchor-rejection retry path
  // alive (a failed data write kills the session), and it makes anchor and
  // data failures distinguishable in the logs.
  bool anchor_confirmed;
  // The rtptime named by this session's anchor - delivery starts exactly at
  // this frame (see the gate in buffered_frame_send). Equals the write head
  // for a first/frozen anchor, or a slightly-future join point when joining
  // an already-anchored master session.
  uint32_t buffered_start_rtptime;
  // Timer + budget for retrying a rejected SETRATEANCHORTIME, see
  // response_handler_send_anchor()
  struct event *anchorev;
  int anchor_retries_left;
  unsigned short buffered_data_port;
  struct airplay_buffered_stream *buffered;

  /* Pairing, see pair.h */
  enum pair_type pair_type;
  struct pair_cipher_context *control_cipher_ctx;
  struct pair_verify_context *pair_verify_ctx;
  struct pair_setup_context *pair_setup_ctx;

  uint8_t shared_secret[64];
  size_t shared_secret_len; // 32 or 64, see AIRPLAY_AUDIO_KEY_LEN for comment

  gcry_cipher_hd_t packet_cipher_hd;

  int server_fd;

  struct airplay_service *timing_svc;
  struct airplay_service *control_svc;

  uint32_t ptpd_slave_id;

  struct airplay_session *next;
};

struct airplay_metadata
{
  struct evbuffer *metadata;
  struct evbuffer *artwork;
  int artwork_fmt;
};

struct airplay_service
{
  struct net_socket socket;
  unsigned short port;
  struct event *ev4;
  struct event *ev6;
};

/* NTP timestamp definitions */
#define FRAC             4294967296. /* 2^32 as a double */
#define NTP_EPOCH_DELTA  0x83aa7e80  /* 2208988800 - that's 1970 - 1900 in seconds */

// TODO move to rtp_common
struct ntp_stamp
{
  uint32_t sec;
  uint32_t frac;
};


/* --------------------------- SEQUENCE DEFINITIONS ------------------------- */

struct airplay_seq_definition
{
  enum airplay_seq_type seq_type;

  // Called when a sequence ends, successfully or not. Shoulds also, if
  // required, take care of notifying  player and free the session.
  void (*on_success)(struct airplay_session *session);
  void (*on_error)(struct airplay_session *session);
};

struct airplay_seq_request
{
  enum airplay_seq_type seq_type;
  const char *name; // Name of request (for logging)
  enum evrtsp_cmd_type rtsp_type;
  int (*payload_make)(struct evrtsp_request *req, struct airplay_session *session, void *arg);
  enum airplay_seq_type (*response_handler)(struct evrtsp_request *req, struct airplay_session *session);
  const char *content_type;
  const char *uri;
  bool proceed_on_rtsp_not_ok; // If true return code != RTSP_OK will not abort the sequence
};

struct airplay_seq_ctx
{
  struct airplay_seq_request *cur_request;
  void (*on_success)(struct airplay_session *session);
  void (*on_error)(struct airplay_session *session);
  struct airplay_session *session;
  void *payload_make_arg;
  const char *log_caller;
};


/* ------------------------------ MISC GLOBALS ------------------------------ */

#if AIRPLAY_USE_AUTH_SETUP
static const uint8_t airplay_auth_setup_pubkey[] =
  "\x59\x02\xed\xe9\x0d\x4e\xf2\xbd\x4c\xb6\x8a\x63\x30\x03\x82\x07"
  "\xa9\x4d\xbd\x50\xd8\xaa\x46\x5b\x5d\x8c\x01\x2a\x0c\x7e\x1d\x4e";
#endif

struct features_type_map
{
  uint32_t bit;
  char *name;
};

// List of features announced by AirPlay 2 speakers
// Credit @invano, see https://emanuelecozzi.net/docs/airplay2
static const struct features_type_map features_map[] =
  {
    { 0, "SupportsAirPlayVideoV1" },
    { 1, "SupportsAirPlayPhoto" },
    { 5, "SupportsAirPlaySlideshow" },
    { 7, "SupportsAirPlayScreen" },
    { 9, "SupportsAirPlayAudio" },
    { 11, "AudioRedunant" },
    { 14, "Authentication_4" }, // FairPlay authentication
    { 15, "MetadataFeatures_0" }, // Send artwork image to receiver
    { 16, "MetadataFeatures_1" }, // Send track progress status to receiver
    { 17, "MetadataFeatures_2" }, // Send NowPlaying info via DAAP
    { 18, "AudioFormats_0" },
    { 19, "AudioFormats_1" },
    { 20, "AudioFormats_2" },
    { 21, "AudioFormats_3" },
    { 23, "Authentication_1" }, // RSA authentication (NA)
    { 26, "Authentication_8" }, // 26 || 51, MFi authentication
    { 27, "SupportsLegacyPairing" },
    { 30, "HasUnifiedAdvertiserInfo" },
    { 32, "IsCarPlay" },
    { 32, "SupportsVolume" }, // !32
    { 33, "SupportsAirPlayVideoPlayQueue" },
    { 34, "SupportsAirPlayFromCloud" }, // 34 && flags_6_SupportsAirPlayFromCloud
    { 35, "SupportsTLS_PSK" },
    { 38, "SupportsUnifiedMediaControl" },
    { 40, "SupportsBufferedAudio" }, // srcvers >= 354.54.6 && 40
    { 41, "SupportsPTP" }, // srcvers >= 366 && 41
    { 42, "SupportsScreenMultiCodec" },
    { 43, "SupportsSystemPairing" },
    { 44, "IsAPValeriaScreenSender" },
    { 46, "SupportsHKPairingAndAccessControl" },
    { 48, "SupportsCoreUtilsPairingAndEncryption" }, // 38 || 46 || 43 || 48
    { 49, "SupportsAirPlayVideoV2" },
    { 50, "MetadataFeatures_3" }, // Send NowPlaying info via bplist
    { 51, "SupportsUnifiedPairSetupAndMFi" },
    { 52, "SupportsSetPeersExtendedMessage" },
    { 54, "SupportsAPSync" },
    { 55, "SupportsWoL" }, // 55 || 56
    { 56, "SupportsWoL" }, // 55 || 56
    { 58, "SupportsHangdogRemoteControl" }, // ((isAppleTV || isAppleAudioAccessory) && 58) || (isThirdPartyTV && flags_10)
    { 59, "SupportsAudioStreamConnectionSetup" }, // 59 && !disableStreamConnectionSetup
    { 60, "SupportsAudioMediaDataControl" }, // 59 && 60 && !disableMediaDataControl
    { 61, "SupportsRFC2198Redundancy" },
  };

/* Keep in sync with enum airplay_devtype */
static const char *airplay_devtype[] =
{
  "AirPort Express 2 - 802.11n",
  "AirPort Express 3 - 802.11n",
  "AppleTV",
  "AppleTV4",
  "HomePod",
  "Other",
};

/* Struct with default quality levels */
static struct media_quality airplay_quality_default =
{
  AIRPLAY_QUALITY_SAMPLE_RATE_DEFAULT,
  AIRPLAY_QUALITY_BITS_PER_SAMPLE_DEFAULT,
  AIRPLAY_QUALITY_CHANNELS_DEFAULT
};

/* From player.c */
extern struct event_base *evbase_player;

/* AirTunes v2 time synchronization */
static struct airplay_service airplay_timing_svc;

/* AirTunes v2 playback synchronization / control */
static struct airplay_service airplay_control_svc;

/* Metadata */
static struct output_metadata *airplay_cur_metadata;

/* Keep-alive timer - hack for ATV's with tvOS 10 */
static struct event *keep_alive_timer;
static struct timeval keep_alive_tv = { AIRPLAY_KEEP_ALIVE_INTERVAL, 0 };

/* Sessions */
static struct airplay_master_session *airplay_master_sessions;
static struct airplay_session *airplay_sessions;

// Encoder admission control (buffered ams only). in_use is charged/released
// on the player thread only, one unit of work at a time, so no lock is
// needed. budget is set once in airplay_init() from the CPU-capability
// score, unless a positive buffered_encoder_budget config override applies.
static int encoder_cost_in_use;
static int encoder_cost_budget;
// Set (and cleared) inside master_session_make() so the caller of a NULL
// return can tell an admission rejection apart from any other setup failure,
// without master_session_make needing a device pointer just for this.
static bool encoder_budget_rejected;

/* Our own device ID, name and user agent */
static uint64_t airplay_device_id;
static const char *airplay_client_name;
static const char *airplay_user_agent;

/* Precision Time Protocol */
static char airplay_ptp_clock_uuid[37];
static bool airplay_ptp_is_disabled;

// Forwards
static int
airplay_device_start(struct output_device *device, int callback_id);
static int
airplay_device_probe(struct output_device *device, int callback_id);
static void
sequence_start(enum airplay_seq_type seq_type, struct airplay_session *session, void *arg, const char *log_caller);
static void
sequence_continue(struct airplay_seq_ctx *seq_ctx);


/* ------------------------------- MISC HELPERS ----------------------------- */

static inline int
alac_encode(struct evbuffer *evbuf, struct encode_ctx *encode_ctx, uint8_t *rawbuf, size_t rawbuf_size, int nsamples, struct media_quality *quality)
{
  transcode_frame *frame;
  int len;

  frame = transcode_frame_new(rawbuf, rawbuf_size, nsamples, quality);
  if (!frame)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not convert raw PCM to frame (bufsize=%zu)\n", rawbuf_size);
      return -1;
    }

  len = transcode_encode(evbuf, encode_ctx, frame, 0);
  transcode_frame_free(frame);
  if (len < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not ALAC encode frame\n");
      return -1;
    }

  return len;
}

// AirTunes v2 time synchronization helpers
static inline void
timespec_to_ntp(struct timespec *ts, struct ntp_stamp *ns)
{
  // Seconds since NTP Epoch (1900-01-01)
  ns->sec = ts->tv_sec + NTP_EPOCH_DELTA;

  ns->frac = (uint32_t)((double)ts->tv_nsec * 1e-9 * FRAC);
}

/*
static inline void
ntp_to_timespec(struct ntp_stamp *ns, struct timespec *ts)
{
  // Seconds since Unix Epoch (1970-01-01)
  ts->tv_sec = ns->sec - NTP_EPOCH_DELTA;

  ts->tv_nsec = (long)((double)ns->frac / (1e-9 * FRAC));
}
*/

static inline int
timing_get_clock_ntp(struct ntp_stamp *ns)
{
  struct timespec ts;
  int ret;

  ret = clock_gettime(CLOCK_MONOTONIC, &ts);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Couldn't get clock: %s\n", strerror(errno));

      return -1;
    }

  timespec_to_ntp(&ts, ns);

  return 0;
}

// Converts uint64t libhash -> AA:BB:CC:DD:EE:FF:11:22
static void
device_id_colon_make(char *id_str, int size, uint64_t id)
{
  int r, w;

  snprintf(id_str, size, "%016" PRIX64, id);

  for (r = strlen(id_str) - 1, w = size - 2; r != w; r--, w--)
    {
      id_str[w] = id_str[r];
      if (r % 2 == 0)
        {
	  w--;
	  id_str[w] = ':';
	}
    }

  id_str[size - 1] = 0; // Zero terminate
}

// Converts AA:BB:CC:DD:EE:FF -> AABBCCDDEEFF -> uint64 id
static int
device_id_colon_parse(uint64_t *id, const char *id_str)
{
  char *s;
  char *ptr;
  int ret;

  CHECK_NULL(L_AIRPLAY, s = calloc(1, strlen(id_str) + 1));

  for (ptr = s; *id_str != '\0'; id_str++)
    {
      if (*id_str == ':')
	continue;

      *ptr = *id_str;
      ptr++;
    }

  ret = safe_hextou64(s, id);
  free(s);

  return ret;
}

static int
device_id_find_byname(uint64_t *id, const char *name)
{
  struct output_device *device;
  struct airplay_extra *extra;

  for (device = outputs_list(); device; device = device->next)
    {
      if (device->type != OUTPUT_TYPE_AIRPLAY)
	continue;

      extra = device->extra_device_info;
      if (strcmp(name, extra->mdns_name) == 0)
	break;
    }

  if (!device)
    return -1;

  *id = device->id;
  return 0;
}


/* ------------------------------- Crypto ----------------------------------- */

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


/* --------------------- Helpers for sending RTSP requests ------------------ */

static int
auth_header_add(struct evrtsp_request *req, const char *password, const char *realm, const char *nonce, const char *method, const char *uri)
{
  const char *hash_fmt = "%02x";
  const char *username = "";
  char ha1[33] = { 0 };
  char ha2[33] = { 0 };
  char ebuf[64];
  char auth[256];
  uint8_t *hash_bytes;
  size_t hashlen;
  gcry_md_hd_t hd;
  gpg_error_t gc_err;
  int i;
  int ret;

  if (!password)
    password = "";

  gc_err = gcry_md_open(&hd, GCRY_MD_MD5, 0);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_AIRPLAY, "Could not open MD5: %s\n", ebuf);
      return -1;
    }

  hashlen = gcry_md_get_algo_dlen(GCRY_MD_MD5);

  /* HA 1 */
  gcry_md_write(hd, username, strlen(username));
  gcry_md_write(hd, ":", 1);
  gcry_md_write(hd, realm, strlen(realm));
  gcry_md_write(hd, ":", 1);
  gcry_md_write(hd, password, strlen(password));

  hash_bytes = gcry_md_read(hd, GCRY_MD_MD5);
  if (!hash_bytes)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not read MD5 hash\n");
      return -1;
    }

  for (i = 0; i < hashlen; i++)
    sprintf(ha1 + (2 * i), hash_fmt, hash_bytes[i]);

  /* RESET */
  gcry_md_reset(hd);

  /* HA 2 */
  gcry_md_write(hd, method, strlen(method));
  gcry_md_write(hd, ":", 1);
  gcry_md_write(hd, uri, strlen(uri));

  hash_bytes = gcry_md_read(hd, GCRY_MD_MD5);
  if (!hash_bytes)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not read MD5 hash\n");
      return -1;
    }

  for (i = 0; i < hashlen; i++)
    sprintf(ha2 + (2 * i), hash_fmt, hash_bytes[i]);

  /* RESET */
  gcry_md_reset(hd);

  /* Final value */
  gcry_md_write(hd, ha1, 32);
  gcry_md_write(hd, ":", 1);
  gcry_md_write(hd, nonce, strlen(nonce));
  gcry_md_write(hd, ":", 1);
  gcry_md_write(hd, ha2, 32);

  hash_bytes = gcry_md_read(hd, GCRY_MD_MD5);
  if (!hash_bytes)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not read MD5 hash\n");
      return -1;
    }

  for (i = 0; i < hashlen; i++)
    sprintf(ha1 + (2 * i), hash_fmt, hash_bytes[i]);

  gcry_md_close(hd);

  /* Build header */
  ret = snprintf(auth, sizeof(auth), "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"",
		 username, realm, nonce, uri, ha1);
  if ((ret < 0) || (ret >= sizeof(auth)))
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Authorization value header exceeds buffer size\n");
      return -1;
    }

  evrtsp_add_header(req->output_headers, "Authorization", auth);

  DPRINTF(E_SPAM, L_AIRPLAY, "Authorization header: %s\n", auth);

  return 0;
}

static int
auth_header_parse(struct airplay_session *session, struct evrtsp_request *req)
{
  const char *param;
  char *auth = NULL;
  char *token;
  char *ptr;

  if (session->realm)
    {
      free(session->realm);
      session->realm = NULL;
    }

  if (session->nonce)
    {
      free(session->nonce);
      session->nonce = NULL;
    }

  param = evrtsp_find_header(req->input_headers, "WWW-Authenticate");
  if (!param)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "WWW-Authenticate header not found\n");
      goto error;
    }

  DPRINTF(E_DBG, L_AIRPLAY, "WWW-Authenticate: %s\n", param);

  if (strncmp(param, "Digest ", strlen("Digest ")) != 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Unsupported authentication method: %s\n", param);
      goto error;
    }

  auth = strdup(param);
  if (!auth)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Out of memory for WWW-Authenticate header copy\n");
      goto error;
    }

  token = strchr(auth, ' ');
  if (!token)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Unexpected WWW-Authenticate auth\n");
      goto error;
    }

  token++;

  token = strtok_r(token, " =", &ptr);
  while (token)
    {
      if (strcmp(token, "realm") == 0)
	{
	  token = strtok_r(NULL, "=\"", &ptr);
	  if (!token)
	    break;

	  session->realm = strdup(token);
	}
      else if (strcmp(token, "nonce") == 0)
	{
	  token = strtok_r(NULL, "=\"", &ptr);
	  if (!token)
	    break;

	  session->nonce = strdup(token);
	}

      token = strtok_r(NULL, " =", &ptr);
    }

  if (!session->realm || !session->nonce)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not find realm/nonce in WWW-Authenticate header\n");

      if (session->realm)
	{
	  free(session->realm);
	  session->realm = NULL;
	}

      if (session->nonce)
	{
	  free(session->nonce);
	  session->nonce = NULL;
	}

      goto error;
    }

  DPRINTF(E_SPAM, L_AIRPLAY, "Found realm: [%s], nonce: [%s]\n", session->realm, session->nonce);

  free(auth);
  return 0;

 error:
  free(auth);
  return -1;
}


static int
request_headers_add(struct evrtsp_request *req, struct airplay_session *session, enum evrtsp_cmd_type req_method)
{
  char buf[64];
  const char *method;
  const char *url;
  int ret;

  snprintf(buf, sizeof(buf), "%d", session->cseq);
  evrtsp_add_header(req->output_headers, "CSeq", buf);

  session->cseq++;

  evrtsp_add_header(req->output_headers, "User-Agent", airplay_user_agent);
  evrtsp_add_header(req->output_headers, "X-Apple-Client-Name", airplay_client_name);

  // If we have a realm + nonce it means that the device told us in the reply to
  // SETUP that www authentication with password is required
  if (session->realm && session->nonce)
    {
      method = evrtsp_method(req_method);
      url = (req_method == EVRTSP_REQ_OPTIONS) ? "*" : session->session_url;

      ret = auth_header_add(req, session->password, session->realm, session->nonce, method, url);
      if (ret < 0)
	return -1;

      session->req_has_auth = 1;
    }

  snprintf(buf, sizeof(buf), "%" PRIX64, libhash);
  evrtsp_add_header(req->output_headers, "Client-Instance", buf);
  evrtsp_add_header(req->output_headers, "DACP-ID", buf);

  // We set Active-Remote as 32 bit unsigned decimal, as at least my device
  // can't handle any larger. Must be aligned with volume_byactiveremote().
  snprintf(buf, sizeof(buf), "%" PRIu32, (uint32_t)session->device_id);
  evrtsp_add_header(req->output_headers, "Active-Remote", buf);

#if AIRPLAY_USE_STREAMID
  evrtsp_add_header(req->output_headers, "X-Apple-StreamID", "1");
#endif

  /* Content-Length added automatically by evrtsp */

  return 0;
}

static void
metadata_rtptimes_get(uint32_t *start, uint32_t *display, uint32_t *pos, uint32_t *end, struct airplay_master_session *ams, struct output_metadata *metadata)
{
  struct rtp_session *rtp_session = ams->rtp_session;
  // All the calculations with long ints to avoid surprises
  int64_t sample_rate;
  int64_t diff_ms;
  int64_t elapsed_ms;
  int64_t elapsed_samples;
  int64_t len_samples;

  sample_rate = rtp_session->quality.sample_rate;

  // First calculate the rtptime that streaming of this item started:
  // - at time metadata->pts the elapsed time was metadata->pos_ms
  // - the time is now ams->cur_stamp.ts and the position is ams->cur_stamp.pos
  // -> time since item started is elapsed_ms = metadata->pos_ms + (ams->cur_stamp.ts - metadata->pts)
  // -> start must then be start = ams->cur_stamp.pos - elapsed_ms * sample_rate;
  diff_ms         = (ams->cur_stamp.ts.tv_sec - metadata->pts.tv_sec) * 1000L + (ams->cur_stamp.ts.tv_nsec - metadata->pts.tv_nsec) / 1000000L;
  elapsed_ms      = (int64_t)metadata->pos_ms + diff_ms;
  elapsed_samples = elapsed_ms * sample_rate / 1000;
  *start          = ams->cur_stamp.pos - elapsed_samples;

/*  DPRINTF(E_DBG, L_AIRPLAY, "pos_ms=%u, len_ms=%u, startup=%d, metadata.pts=%ld.%09ld, player.ts=%ld.%09ld, diff_ms=%" PRIi64 ", elapsed_ms=%" PRIi64 "\n",
    metadata->pos_ms, metadata->len_ms, metadata->startup, metadata->pts.tv_sec, metadata->pts.tv_nsec, ams->cur_stamp.ts.tv_sec, ams->cur_stamp.ts.tv_nsec, diff_ms, elapsed_ms);
*/
  // Here's the deal with progress values:
  // - display is always start minus a delay
  //    -> delay x1 if streaming is starting for this device (joining or not)
  //    -> delay x2 if stream is switching to a new song
  //    TODO what if we are just sending a keep_alive?
  // - pos is the RTP time of the first sample for this song for this device
  //    -> start of song
  //    -> start of song + offset if device is joining in the middle of a song,
  //       or getting out of a pause or seeking
  // - end is the RTP time of the last sample for this song
  len_samples     = (int64_t)metadata->len_ms * sample_rate / 1000;
  *display        = metadata->startup ? *start - AIRPLAY_MD_DELAY_STARTUP : *start - AIRPLAY_MD_DELAY_SWITCH;
  *pos            = MAX(ams->cur_stamp.pos, *start);
  *end            = len_samples ? *start + len_samples : *pos;

  DPRINTF(E_SPAM, L_AIRPLAY, "start=%u, display=%u, pos=%u, end=%u, rtp_session.pos=%u, cur_stamp.pos=%u\n",
    *start, *display, *pos, *end, rtp_session->pos, ams->cur_stamp.pos);
}

static int
rtpinfo_header_add(struct evrtsp_request *req, struct airplay_session *session, struct output_metadata *metadata)
{
  uint32_t start;
  uint32_t display;
  uint32_t pos;
  uint32_t end;
  char rtpinfo[32];
  int ret;

  metadata_rtptimes_get(&start, &display, &pos, &end, session->master_session, metadata);

  ret = snprintf(rtpinfo, sizeof(rtpinfo), "rtptime=%u", start);
  if ((ret < 0) || (ret >= sizeof(rtpinfo)))
    {
      DPRINTF(E_LOG, L_AIRPLAY, "RTP-Info too big for buffer while sending metadata\n");
      return -1;
    }

  evrtsp_add_header(req->output_headers, "RTP-Info", rtpinfo);
  return 0;
}

static int
rtsp_cipher(struct evbuffer *outbuf, struct evbuffer *inbuf, void *arg, int encrypt)
{
  struct airplay_session *session = arg;
  uint8_t *in;
  size_t in_len;
  uint8_t *out = NULL;
  size_t out_len = 0;
  ssize_t processed;

  in = evbuffer_pullup(inbuf, -1);
  in_len = evbuffer_get_length(inbuf);

  if (encrypt)
    {
#if AIRPLAY_DUMP_TRAFFIC
      if (in_len < 4096)
	DHEXDUMP(E_DBG, L_AIRPLAY, in, in_len, "Encrypting outgoing request\n");
      else
	DPRINTF(E_DBG, L_AIRPLAY, "Encrypting outgoing request (size %zu)\n", in_len);
#endif

      processed = pair_encrypt(&out, &out_len, in, in_len, session->control_cipher_ctx);
      if (processed < 0)
	goto error;
    }
  else
    {
      processed = pair_decrypt(&out, &out_len, in, in_len, session->control_cipher_ctx);
      if (processed < 0)
	goto error;

#if AIRPLAY_DUMP_TRAFFIC
      if (out_len < 4096)
	DHEXDUMP(E_DBG, L_AIRPLAY, out, out_len, "Decrypted incoming response\n");
      else
	DPRINTF(E_DBG, L_AIRPLAY, "Decrypted incoming response (size %zu)\n", out_len);
#endif
    }

  evbuffer_drain(inbuf, processed);
  evbuffer_add(outbuf, out, out_len);

  return 0;

 error:
  DPRINTF(E_LOG, L_AIRPLAY, "Error while %s (len=%zu): %s\n", encrypt ? "encrypting" : "decrypting", in_len, pair_cipher_errmsg(session->control_cipher_ctx));

  return -1;
}


/* ------------------------------ Session handling -------------------------- */

// Maps our internal state to the generic output state and then makes a callback
// to the player to tell that state
static void
session_status(struct airplay_session *session)
{
  enum output_device_state state;

  switch (session->state)
    {
      case AIRPLAY_STATE_AUTH:
	state = OUTPUT_STATE_PASSWORD;
	break;
      case AIRPLAY_STATE_FAILED:
	state = OUTPUT_STATE_FAILED;
	break;
      case AIRPLAY_STATE_STOPPED:
	state = OUTPUT_STATE_STOPPED;
	break;
      case AIRPLAY_STATE_INFO ... AIRPLAY_STATE_RECORD:
	state = OUTPUT_STATE_STARTUP;
	break;
      case AIRPLAY_STATE_CONNECTED:
	state = OUTPUT_STATE_CONNECTED;
	break;
      case AIRPLAY_STATE_STREAMING:
	state = OUTPUT_STATE_STREAMING;
	break;
      case AIRPLAY_STATE_TEARDOWN:
	DPRINTF(E_LOG, L_AIRPLAY, "Bug! session_status() called with transitional state (TEARDOWN)\n");
	state = OUTPUT_STATE_STOPPED;
	break;
      default:
	DPRINTF(E_LOG, L_AIRPLAY, "Bug! Unhandled state in session_status(): %d\n", session->state);
	state = OUTPUT_STATE_FAILED;
    }

  outputs_cb(session->callback_id, session->device_id, state);
  session->callback_id = -1;
}

static void
master_session_free(struct airplay_master_session *ams)
{
  if (!ams)
    return;

  // Release the admission charge exactly once, here. master_session_make()
  // only assigns buffered_kind on the success path (after the charge), so an
  // ams freed on an error path was never charged and this is a no-op for it.
  if (ams->buffered_kind != AIRPLAY_BUFFERED_KIND_NONE)
    {
      int cost = (ams->buffered_kind == AIRPLAY_BUFFERED_KIND_SURROUND_UPMIX) ? ENCODER_COST_SURROUND_UPMIX : ENCODER_COST_DEFAULT;
      encoder_cost_in_use -= cost;
    }

  // Join the encoder thread before touching anything it might still be using.
  airplay_encoder_stop(&ams->encoder);

  outputs_quality_unsubscribe(&ams->rtp_session->quality);
  rtp_session_free(ams->rtp_session);

  transcode_encode_cleanup(&ams->encode_ctx);

  if (ams->input_buffer)
    evbuffer_free(ams->input_buffer);
  if (ams->encoded_buffer)
    evbuffer_free(ams->encoded_buffer);

  free(ams->rawbuf);
  free(ams);
}

static void
master_session_cleanup(struct airplay_master_session *ams)
{
  struct airplay_master_session *s;
  struct airplay_session *session;

  // First check if any other session is using the master session
  for (session = airplay_sessions; session; session=session->next)
    {
      if (session->master_session == ams)
	return;
    }

  if (ams == airplay_master_sessions)
    airplay_master_sessions = airplay_master_sessions->next;
  else
    {
      for (s = airplay_master_sessions; s && (s->next != ams); s = s->next)
	; /* EMPTY */

      if (!s)
	DPRINTF(E_WARN, L_AIRPLAY, "WARNING: struct airplay_master_session not found in list; BUG!\n");
      else
	s->next = ams->next;
    }

  master_session_free(ams);
}

static struct airplay_master_session *
master_session_make(struct media_quality *quality, bool use_ptp, enum airplay_buffered_kind kind)
{
  struct airplay_master_session *ams;
  struct media_quality alac24_quality;
  uint64_t buffer_duration_ms;
  bool buffered = (kind != AIRPLAY_BUFFERED_KIND_NONE);
  bool alac24 = (kind == AIRPLAY_BUFFERED_KIND_ALAC24);
  enum transcode_profile profile;
  struct transcode_encode_setup_args encode_args;
  uint64_t clock_id;
  int ret;
  int cost;
  int budget;

  // For the ALAC 48k/24 profile the session subscribes at 48k/32-bit/2ch
  // instead of the device default, so a >16-bit source reaches the encoder
  // without truncation and without a resample away from 48k. 16-bit sources
  // arrive converted/zero-padded by the player, same audible result as
  // before. Must be set before the reuse check below so equality compares the
  // real subscription quality.
  if (alac24)
    {
      memset(&alac24_quality, 0, sizeof(alac24_quality));
      alac24_quality.sample_rate = 48000;
      alac24_quality.bits_per_sample = 32;
      alac24_quality.channels = 2;
      quality = &alac24_quality;
    }

  encoder_budget_rejected = false;

  // First check if we already have a suitable session
  for (ams = airplay_master_sessions; ams; ams = ams->next)
    {
      if (quality_is_equal(quality, &ams->rtp_session->quality) && use_ptp == ams->use_ptp
          && kind == ams->buffered_kind)
	return ams;
    }

  // Admission control: reject before anything is allocated. Charge happens
  // later, only on the success path (see ams->buffered_kind assignment
  // below); release happens solely in master_session_free(). A positive
  // buffered_encoder_budget config override replaces the computed budget for
  // this check only - re-read every time so a runtime change takes effect on
  // the next activation without a restart.
  if (buffered)
    {
      cost = (kind == AIRPLAY_BUFFERED_KIND_SURROUND_UPMIX) ? ENCODER_COST_SURROUND_UPMIX : ENCODER_COST_DEFAULT;
      budget = config_get_int("buffered_encoder_budget", 0);
      if (budget <= 0)
        budget = encoder_cost_budget;

      if (encoder_cost_in_use + cost > budget)
        {
          DPRINTF(E_LOG, L_AIRPLAY, "Refusing new buffered output (kind %d, cost %d): encoder budget exhausted (%d/%d in use)\n",
                  kind, cost, encoder_cost_in_use, budget);
          encoder_budget_rejected = true;
          return NULL;
        }
    }

  // Each buffered kind uses its own fixed-format encode profile instead of
  // realtime ALAC; everything else about master session creation (input
  // quality subscribe, rawbuf sized for AIRPLAY_SAMPLES_PER_PACKET input
  // samples, rtp_session) is unchanged - the rtp_session is kept for input
  // quality subscription and metadata rtptime calculations, buffered packets
  // don't use rtp_packet_next.
  switch (kind)
    {
      case AIRPLAY_BUFFERED_KIND_ALAC24:           profile = XCODE_ALAC48K_24_STEREO; break;
      case AIRPLAY_BUFFERED_KIND_SURROUND_STEREO:  profile = XCODE_AAC48K_51_STEREO;  break;
      case AIRPLAY_BUFFERED_KIND_SURROUND_UPMIX:   profile = XCODE_AAC48K_51_DECODE;  break;
      case AIRPLAY_BUFFERED_KIND_AAC_STEREO:       profile = XCODE_AAC48K_STEREO;     break;
      case AIRPLAY_BUFFERED_KIND_NONE:
      default:                                     profile = XCODE_ALAC;              break;
    }
  encode_args = (struct transcode_encode_setup_args){ .profile = profile, .quality = quality };

  // Let's create a master session
  ret = outputs_quality_subscribe(quality);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not subscribe to required audio quality (%d/%d/%d)\n", quality->sample_rate, quality->bits_per_sample, quality->channels);
      return NULL;
    }

  CHECK_NULL(L_AIRPLAY, ams = calloc(1, sizeof(struct airplay_master_session)));

  clock_id = use_ptp ? ptpd_clock_id_get() : 0;

  ams->rtp_session = rtp_session_new(quality, AIRPLAY_PACKET_BUFFER_SIZE, 0, clock_id);
  if (!ams->rtp_session)
    {
      goto error;
    }

  // Buffered receivers select their decoder from the RTP SSRC (see
  // AIRPLAY_SSRC_ID_ALAC48K_24). rtp_session_new() leaves ssrc_id 0 on PTP
  // sessions, which is fine for AAC but means ALAC frames are never decoded.
  if (alac24)
    ams->rtp_session->ssrc_id = AIRPLAY_SSRC_ID_ALAC48K_24;

  if (buffered)
    {
      // The encode context and its PCM/frame queues live on the encoder's own
      // thread from here on; the ams never touches them directly.
      if (airplay_encoder_start(&ams->encoder, kind, quality) < 0)
	{
	  DPRINTF(E_LOG, L_AIRPLAY, "Will not be able to stream AirPlay 2, could not start buffered encoder (kind %d)\n", kind);
	  goto error;
	}
    }
  else
    {
      encode_args.src_ctx = transcode_decode_setup_raw(alac24 ? XCODE_PCM32 : XCODE_PCM16, quality);
      if (!encode_args.src_ctx)
	{
	  DPRINTF(E_LOG, L_AIRPLAY, "Could not create decoding context\n");
	  goto error;
	}

      ams->encode_ctx = transcode_encode_setup(encode_args);
      transcode_decode_cleanup(&encode_args.src_ctx);
      if (!ams->encode_ctx)
	{
	  DPRINTF(E_LOG, L_AIRPLAY, "Will not be able to stream AirPlay 2, ffmpeg has no ALAC encoder\n");
	  goto error;
	}
    }

  buffer_duration_ms = outputs_buffer_duration_ms_get();
  if (buffer_duration_ms <= AIRPLAY_AUDIO_LATENCY_MS)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Configuration of start_buffer_ms must be higher than min latency (%d)\n", AIRPLAY_AUDIO_LATENCY_MS);
      goto error;
    }

  ams->quality = *quality;
  ams->use_ptp = use_ptp;
  ams->samples_per_packet = AIRPLAY_SAMPLES_PER_PACKET;
  ams->rawbuf_size = STOB(ams->samples_per_packet, quality->bits_per_sample, quality->channels);
  ams->output_buffer_samples = (buffer_duration_ms - AIRPLAY_AUDIO_LATENCY_MS) * quality->sample_rate / 1000;

  ams->buffered = buffered;
  ams->buffered_kind = kind;
  if (buffered)
    {
      encoder_cost_in_use += cost;
      DPRINTF(E_INFO, L_AIRPLAY, "Buffered encoder (kind %d, cost %d) admitted: %d/%d in use\n", kind, cost, encoder_cost_in_use, budget);
    }
  ams->buffered_spf = alac24 ? AIRPLAY_BUFFERED_SPF_ALAC24 : AIRPLAY_BUFFERED_SPF;
  ams->buffered_seqnum = 0;
  ams->buffered_rtptime = 0;
  ams->buffered_marker_pending = true;
  ams->buffered_anchor_mapped = false;

  if (!buffered)
    {
      CHECK_NULL(L_AIRPLAY, ams->rawbuf = malloc(ams->rawbuf_size));
      CHECK_NULL(L_AIRPLAY, ams->input_buffer = evbuffer_new());
      CHECK_NULL(L_AIRPLAY, ams->encoded_buffer = evbuffer_new());
    }

  ams->next = airplay_master_sessions;
  airplay_master_sessions = ams;

  return ams;

 error:
  master_session_free(ams);
  return NULL;
}

static void
session_free(struct airplay_session *session)
{
  if (!session)
    return;

  if (session->master_session)
    master_session_cleanup(session->master_session);

  if (session->ctrl)
    {
      evrtsp_connection_set_closecb(session->ctrl, NULL, NULL);
      evrtsp_connection_free(session->ctrl);
    }

  if (session->deferredev)
    event_free(session->deferredev);

  if (session->anchorev)
    event_free(session->anchorev);

  airplay_buffered_stop(session->buffered);

  if (session->server_fd >= 0)
    close(session->server_fd);

  if (session->ptpd_slave_id > 0)
    ptpd_slave_remove(session->ptpd_slave_id);

  chacha_close(session->packet_cipher_hd);

  pair_setup_free(session->pair_setup_ctx);
  pair_verify_free(session->pair_verify_ctx);
  pair_cipher_free(session->control_cipher_ctx);

  free(session->local_v4_address);
  free(session->local_v6_address);
  free(session->realm);
  free(session->nonce);
  free(session->address);
  free(session->devname);

  free(session);
}

static void
session_cleanup(struct airplay_session *session)
{
  struct airplay_session *s;

  if (session == airplay_sessions)
    airplay_sessions = airplay_sessions->next;
  else
    {
      for (s = airplay_sessions; s && (s->next != session); s = s->next)
	; /* EMPTY */

      if (!s)
	DPRINTF(E_WARN, L_AIRPLAY, "WARNING: struct airplay_session not found in list; BUG!\n");
      else
	s->next = session->next;
    }

  outputs_device_session_remove(session->device_id);

  session_free(session);
}

static void
session_failure(struct airplay_session *session)
{
  /* Session failed, let our user know */
  if (session->state != AIRPLAY_STATE_AUTH)
    session->state = AIRPLAY_STATE_FAILED;

  session_status(session);

  session_cleanup(session);
}

static void
deferred_session_failure_cb(int fd, short what, void *arg)
{
  struct airplay_session *session = arg;

  DPRINTF(E_DBG, L_AIRPLAY, "Cleaning up failed session (deferred) on device '%s'\n", session->devname);
  session_failure(session);
}

static void
deferred_session_failure(struct airplay_session *session)
{
  struct timeval tv;

  if (session->state != AIRPLAY_STATE_AUTH)
    session->state = AIRPLAY_STATE_FAILED;

  evutil_timerclear(&tv);
  evtimer_add(session->deferredev, &tv);
}

// Retries a SETRATEANCHORTIME that the receiver rejected (see
// response_handler_send_anchor). If the session was flushed or torn down
// while the timer was pending, the regular anchor_pending machinery owns the
// next anchor and this retry must not fire a stale one.
static void
anchor_retry_cb(int fd, short what, void *arg)
{
  struct airplay_session *session = arg;

  if (session->state != AIRPLAY_STATE_STREAMING || !session->buffered_mode || session->anchor_pending)
    return;

  sequence_start(AIRPLAY_SEQ_SEND_ANCHOR, session, NULL, "anchor_retry");
}

static void
rtsp_close_cb(struct evrtsp_connection *evcon, void *arg)
{
  struct airplay_session *session = arg;

  DPRINTF(E_LOG, L_AIRPLAY, "Device '%s' closed RTSP connection\n", session->devname);

  deferred_session_failure(session);
}

static void
session_success(struct airplay_session *session)
{
  session_status(session);

  session_cleanup(session);
}

static void
session_connected(struct airplay_session *session)
{
  struct output_device *device;

  session->state = AIRPLAY_STATE_CONNECTED;

  device = outputs_device_get(session->device_id);
  if (device)
    device->last_failure = OUTPUT_FAILURE_NONE;

  session_status(session);
}

static void
session_pair_success(struct airplay_session *session)
{
  if (session->next_seq != AIRPLAY_SEQ_CONTINUE)
    {
      sequence_start(session->next_seq, session, NULL, "pair_success");
      session->next_seq = AIRPLAY_SEQ_CONTINUE;
      return;
    }

  session_success(session);
}

static int
session_connection_setup(struct airplay_session *session, struct output_device *device, int family)
{
  char *address;
  char *intf;
  unsigned short port;
  int ret;

  session->naddr.ss.ss_family = family;

  switch (family)
    {
      case AF_INET:
	if (!device->v4_address)
	  return -1;

	address = device->v4_address;
	port = device->v4_port;


	ret = inet_pton(AF_INET, address, &session->naddr.sin.sin_addr);
	break;

      case AF_INET6:
	if (!device->v6_address || device->v6_disabled)
	  return -1;

	address = device->v6_address;
	port = device->v6_port;

	intf = strchr(address, '%');
	if (intf)
	  *intf = '\0';

	ret = inet_pton(AF_INET6, address, &session->naddr.sin6.sin6_addr);

	if (intf)
	  {
	    *intf = '%';

	    intf++;

	    session->naddr.sin6.sin6_scope_id = if_nametoindex(intf);
	    if (session->naddr.sin6.sin6_scope_id == 0)
	      {
		DPRINTF(E_LOG, L_AIRPLAY, "Could not find interface %s\n", intf);

		ret = -1;
		break;
	      }
	  }

	break;

      default:
	return -1;
    }

  if (ret <= 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Device '%s' has invalid address (%s) for %s\n", device->name, address, (family == AF_INET) ? "ipv4" : "ipv6");
      return -1;
    }

  session->ctrl = evrtsp_connection_new(address, port);
  if (!session->ctrl)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not create control connection to '%s' (%s)\n", device->name, address);
      return -1;
    }

  evrtsp_connection_set_base(session->ctrl, evbase_player);

  session->address = strdup(address);
  session->family = family;

  return 0;
}

static int
session_cipher_setup(struct airplay_session *session, const uint8_t *key, size_t key_len)
{
  struct pair_cipher_context *control_cipher_ctx = NULL;
  gcry_cipher_hd_t packet_cipher_hd = NULL;

  // For transient pairing the key_len will be 64 bytes, and session->shared_secret is 32 bytes
  if (key_len < AIRPLAY_AUDIO_KEY_LEN || key_len > sizeof(session->shared_secret))
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Ciphering setup error: Unexpected key length (%zu)\n", key_len);
      goto error;
    }

  session->shared_secret_len = key_len;
  memcpy(session->shared_secret, key, key_len);

  control_cipher_ctx = pair_cipher_new(session->pair_type, 0, key, key_len);
  if (!control_cipher_ctx)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not create control ciphering context\n");
      goto error;
    }

  packet_cipher_hd = chacha_open(session->shared_secret, AIRPLAY_AUDIO_KEY_LEN);
  if (!packet_cipher_hd)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not create packet ciphering handle\n");
      goto error;
    }

  DPRINTF(E_DBG, L_AIRPLAY, "Ciphering setup of '%s' completed succesfully, now using encrypted mode\n", session->devname);

  session->state = AIRPLAY_STATE_ENCRYPTED;
  session->control_cipher_ctx = control_cipher_ctx;
  session->packet_cipher_hd = packet_cipher_hd;

  evrtsp_connection_set_ciphercb(session->ctrl, rtsp_cipher, session);

  return 0;

 error:
  pair_cipher_free(control_cipher_ctx);
  chacha_close(packet_cipher_hd);
  return -1;
}

static int
session_ids_set(struct airplay_session *session)
{
  char *address = NULL;
  char *intf;
  char ifname[64];
  uint8_t mac[6];
  unsigned short port;
  int family;
  int ret;

  // Determine local address, needed for session URL
  evrtsp_connection_get_local_address(session->ctrl, &address, &port, &family);
  if (!address || (port == 0))
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not determine local v4 address\n");
      goto error;
    }

  intf = strchr(address, '%');
  if (intf)
    {
      *intf = '\0';
      intf++;
    }
  else
    {
      ret = net_if_get(ifname, sizeof(ifname), address);
      intf = (ret < 0) ? NULL : ifname;
    }

  ret = net_mac_get(mac, sizeof(mac), intf);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not determine local MAC address\n");
      goto error;
    }

  ret = snprintf(session->local_mac_address, sizeof(session->local_mac_address), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  if ((ret < 0) || (ret >= sizeof(session->local_mac_address)))
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Session URL length exceeds 127 characters\n");
      goto error;
    }

  DPRINTF(E_DBG, L_AIRPLAY, "Local address: %s port %d, if %s, mac %s\n", address, port, intf, session->local_mac_address);

  // Session UUID, ID and session URL
  uuid_make(session->session_uuid);
  uuid_make(session->group_uuid);

  gcry_randomize(&session->session_id, sizeof(session->session_id), GCRY_STRONG_RANDOM);
  gcry_randomize(&session->stream_connection_id, sizeof(session->stream_connection_id), GCRY_STRONG_RANDOM);

  ret = snprintf(session->session_url, sizeof(session->session_url), (family == AF_INET) ? "rtsp://%s/%u" : "rtsp://[%s]/%u", address, session->session_id);
  if ((ret < 0) || (ret >= sizeof(session->session_url)))
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Session URL length exceeds 127 characters\n");
      goto error;
    }

  if (family == AF_INET)
    session->local_v4_address = address;
  else
    session->local_v6_address = address;

  return 0;

 error:
  free(address);
  return -1;
}

static struct airplay_session *
session_find_by_address(union net_sockaddr *peer_addr)
{
  struct airplay_session *session;
  uint32_t *addr_ptr;
  int family = peer_addr->sa.sa_family;

  for (session = airplay_sessions; session; session = session->next)
    {
      if (family == session->family)
	{
	  if (family == AF_INET && peer_addr->sin.sin_addr.s_addr == session->naddr.sin.sin_addr.s_addr)
	    break;

	  if (family == AF_INET6 && IN6_ARE_ADDR_EQUAL(&peer_addr->sin6.sin6_addr, &session->naddr.sin6.sin6_addr))
	    break;
	}
      else if (family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&peer_addr->sin6.sin6_addr))
	{
	  // ipv4 mapped to ipv6 consists of 16 bytes/4 words: 0x00000000 0x00000000 0x0000ffff 0x[IPv4]
	  addr_ptr = (uint32_t *)(&peer_addr->sin6.sin6_addr);
	  if (addr_ptr[3] == session->naddr.sin.sin_addr.s_addr)
	    break;
	}
    }

  return session;
}

static struct airplay_session *
session_make(struct output_device *device, int callback_id)
{
  struct airplay_session *session;
  struct airplay_extra *extra;
  int ret;

  extra = device->extra_device_info;

  CHECK_NULL(L_AIRPLAY, session = calloc(1, sizeof(struct airplay_session)));
  CHECK_NULL(L_AIRPLAY, session->deferredev = evtimer_new(evbase_player, deferred_session_failure_cb, session));
  CHECK_NULL(L_AIRPLAY, session->anchorev = evtimer_new(evbase_player, anchor_retry_cb, session));

  session->anchor_retries_left = AIRPLAY_BUFFERED_ANCHOR_RETRIES_MAX;

  session->devname = strdup(device->name);
  session->volume = device->volume;
  session->offset_samples = device->offset_ms * device->quality.sample_rate / 1000;

  session->state = AIRPLAY_STATE_STOPPED;
  session->reqs_in_flight = 0;
  session->cseq = 1;

  session->device_id = device->id;
  session->callback_id = callback_id;

  session->server_fd = -1;

  session->password = device->password;

  session->supports_auth_setup = extra->supports_auth_setup;
  session->wanted_metadata = extra->wanted_metadata;
  session->supports_encryption = extra->supports_encryption;

  // Buffered AirPlay 2 opt-in, derived from the device's selected output
  // mode. wants_buffered_kind is what response_handler_info_generic() checks
  // to decide whether to attempt the buffered transport at all, and which
  // profile to request if so. A surround mode selected on a device that
  // doesn't pass the standalone-ATV gate simply leaves the kind at NONE, i.e.
  // falls through to the realtime path with no error.
  {
    enum output_mode mode = device->preferred_mode;
    enum airplay_buffered_kind kind = AIRPLAY_BUFFERED_KIND_NONE;

    if (mode == OUTPUT_MODE_AIRPLAY2_BUFFERED)
      kind = AIRPLAY_BUFFERED_KIND_AAC_STEREO;
    else if (mode == OUTPUT_MODE_AIRPLAY2_BUFFERED_24)
      kind = AIRPLAY_BUFFERED_KIND_ALAC24;
    else if (mode == OUTPUT_MODE_AIRPLAY2_SURROUND_STEREO && airplay_device_allows_surround(device))
      kind = AIRPLAY_BUFFERED_KIND_SURROUND_STEREO;
    else if (mode == OUTPUT_MODE_AIRPLAY2_SURROUND_UPMIX && airplay_device_allows_surround(device))
      kind = AIRPLAY_BUFFERED_KIND_SURROUND_UPMIX;

    // Under "auto", the global buffered-audio switch opts every AirPlay 2
    // device with a learned AAC-buffered capability into the buffered
    // transport ahead of realtime; explicit selections are never influenced
    // by the switch. Surround is never selected implicitly under "auto".
    if (mode == OUTPUT_MODE_AUTO && config_get_bool("buffered_audio_enabled", false)
        && (device->supported_modes & OUTPUT_MODE_AIRPLAY2_BUFFERED))
      kind = AIRPLAY_BUFFERED_KIND_AAC_STEREO;

    session->wants_buffered_kind = kind;
  }
  session->buffered_mode = false;
  session->buffered_format_id = buffered_format_id_for_kind(session->wants_buffered_kind);

  session->next_seq = AIRPLAY_SEQ_CONTINUE;
  session->timing_svc = &airplay_timing_svc;
  session->control_svc = &airplay_control_svc;

  ret = session_connection_setup(session, device, AF_INET6);
  if (ret < 0)
    {
      ret = session_connection_setup(session, device, AF_INET);
      if (ret < 0)
	goto error;
    }

  // Always starts out on the realtime ALAC master session; if the device is
  // configured for buffered mode and the device's /info response confirms
  // support, response_handler_info_generic() swaps this for a buffered one.
  session->master_session = master_session_make(&device->quality, extra->use_ptp, AIRPLAY_BUFFERED_KIND_NONE);
  if (!session->master_session)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not attach a master session for device '%s'\n", device->name);
      goto error;
    }

  DPRINTF(E_DBG, L_AIRPLAY,
          "Stream mode for '%s': payload_type=%u (realtime), timing=%s, reason=%s\n",
          session->devname, AIRPLAY_RTP_PAYLOADTYPE, session->master_session->use_ptp ? "PTP" : "NTP",
          session->wants_buffered_kind != AIRPLAY_BUFFERED_KIND_NONE ? "buffered_configured_pending_capability_check" : "default_sender_path");
  DPRINTF(E_DBG, L_AIRPLAY, "Session group UUID for '%s': %s (source=generated)\n",
          session->devname, session->group_uuid);

  // Attach to list of sessions
  session->next = airplay_sessions;
  airplay_sessions = session;

  // rs is now the official device session
  outputs_device_session_add(device->id, session);

  return session;

 error:
  session_free(session);

  return NULL;
}


/* ----------------------------- Metadata handling -------------------------- */

static void
airplay_metadata_free(struct airplay_metadata *rmd)
{
  if (!rmd)
    return;

  if (rmd->metadata)
    evbuffer_free(rmd->metadata);
  if (rmd->artwork)
    evbuffer_free(rmd->artwork);

  free(rmd);
}

static void
airplay_metadata_purge(void)
{
  if (!airplay_cur_metadata)
    return;

  airplay_metadata_free(airplay_cur_metadata->priv);
  free(airplay_cur_metadata);
  airplay_cur_metadata = NULL;
}

// *** Thread: worker ***
static void *
airplay_metadata_prepare(struct output_metadata *metadata)
{
  struct db_queue_item *queue_item;
  struct airplay_metadata *rmd;
  struct evbuffer *tmp;
  int ret;

  queue_item = db_queue_fetch_byitemid(metadata->item_id);
  if (!queue_item)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not fetch queue item\n");
      return NULL;
    }

  CHECK_NULL(L_AIRPLAY, rmd = calloc(1, sizeof(struct airplay_metadata)));
  CHECK_NULL(L_AIRPLAY, rmd->artwork = evbuffer_new());
  CHECK_NULL(L_AIRPLAY, rmd->metadata = evbuffer_new());
  CHECK_NULL(L_AIRPLAY, tmp = evbuffer_new());

  ret = artwork_get_by_queue_item_id(rmd->artwork, queue_item->id, ART_DEFAULT_WIDTH, ART_DEFAULT_HEIGHT, 0);
  if (ret < 0)
    {
      DPRINTF(E_INFO, L_AIRPLAY, "Failed to retrieve artwork for file '%s'; no artwork will be sent\n", queue_item->path);
      evbuffer_free(rmd->artwork);
      rmd->artwork = NULL;
    }

  rmd->artwork_fmt = ret;

  ret = dmap_encode_queue_metadata(rmd->metadata, tmp, queue_item);
  evbuffer_free(tmp);
  free_queue_item(queue_item, 0);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not encode file metadata; metadata will not be sent\n");
      airplay_metadata_free(rmd);
      return NULL;
    }

  return rmd;
}

static int
airplay_metadata_send_generic(struct airplay_session *session, struct output_metadata *metadata, bool only_progress)
{
  struct airplay_metadata *rmd = metadata->priv;

  if (session->wanted_metadata & AIRPLAY_MD_WANTS_PROGRESS)
    {
      DPRINTF(E_DBG, L_AIRPLAY, "SET_PARAMETER (progress): Sending SET_PARAMETER (progress) to '%s'\n",
              session->devname);
      sequence_start(AIRPLAY_SEQ_SEND_PROGRESS, session, metadata, "SET_PARAMETER (progress)");
    }

  if (!only_progress && (session->wanted_metadata & AIRPLAY_MD_WANTS_TEXT))
    {
      DPRINTF(E_DBG, L_AIRPLAY, "SET_PARAMETER (text): Sending SET_PARAMETER (text) to '%s'\n",
              session->devname);
      sequence_start(AIRPLAY_SEQ_SEND_TEXT, session, metadata, "SET_PARAMETER (text)");
    }

  if (!only_progress && (session->wanted_metadata & AIRPLAY_MD_WANTS_ARTWORK) && rmd->artwork)
    sequence_start(AIRPLAY_SEQ_SEND_ARTWORK, session, metadata, "SET_PARAMETER (artwork)");

  return 0;
}

static int
airplay_metadata_startup_send(struct airplay_session *session)
{
  if (!session->wanted_metadata || !airplay_cur_metadata)
    return 0;

  airplay_cur_metadata->startup = true;

  return airplay_metadata_send_generic(session, airplay_cur_metadata, false);
}

static void
airplay_metadata_keep_alive_send(struct airplay_session *session)
{
  sequence_start(AIRPLAY_SEQ_FEEDBACK, session, NULL, "keep_alive");
}

static void
airplay_metadata_send(struct output_metadata *metadata)
{
  struct airplay_session *session;
  struct airplay_session *next;
  int ret;

  for (session = airplay_sessions; session; session = next)
    {
      next = session->next;

      if (!(session->state & AIRPLAY_STATE_F_CONNECTED) || !session->wanted_metadata)
	continue;

      ret = airplay_metadata_send_generic(session, metadata, false);
      if (ret < 0)
	{
	  session_failure(session);
	  continue;
	}
    }

  // Replace current metadata with the new stuff
  airplay_metadata_purge();
  airplay_cur_metadata = metadata;
}


/* ------------------------------ Volume handling --------------------------- */

static int
volume_max_get(const char *name)
{
  int max_volume = AIRPLAY_CONFIG_MAX_VOLUME;
  cfg_t *airplay;

  airplay = cfg_gettsec(cfg, "airplay", name);
  if (airplay)
    max_volume = cfg_getint(airplay, "max_volume");

  // 0 means max_volume is not set in the config (the settings backend returns
  // 0 for missing keys), so use the default without complaining
  if (max_volume == 0)
    return AIRPLAY_CONFIG_MAX_VOLUME;

  if ((max_volume < 1) || (max_volume > AIRPLAY_CONFIG_MAX_VOLUME))
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Config has bad max_volume (%d) for device '%s', using default instead\n", max_volume, name);
      return AIRPLAY_CONFIG_MAX_VOLUME;
    }

  return max_volume;
}

static float
airplay_volume_from_pct(int volume, const char *name)
{
  float airplay_volume;
  int max_volume;

  max_volume = volume_max_get(name);

  /* RAOP volume
   *  -144.0 is off (not really used since we have no concept of muted/off)
   *  0 - 100 maps to -30.0 - 0 (if no max_volume set)
   */
  if (volume >= 0 && volume <= 100)
    airplay_volume = -30.0 + ((float)max_volume * (float)volume * 30.0) / (100.0 * AIRPLAY_CONFIG_MAX_VOLUME);
  else
    airplay_volume = -144.0;

  return airplay_volume;
}

static int
airplay_volume_to_pct(struct output_device *device, const char *volstr)
{
  float airplay_volume;
  float volume;
  int max_volume;

  airplay_volume = atof(volstr);

  if ((airplay_volume == 0.0 && volstr[0] != '0') || airplay_volume > 0.0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "AirPlay device volume is invalid: '%s'\n", volstr);
      return -1;
    }

  if (airplay_volume <= -30.0)
    {
      return 0; // -144.0 is muted
    }

  max_volume = volume_max_get(device->name);

/*
  This is an attempt at scaling the input volume that didn't really work for all
  speakers (e.g. my Sony), but I'm leaving it here in case it should be a config
  option some time

  // If the input volume is -25 and we are playing at -20, then we only want the
  // resulting volume to be -25 if there is no volume scaling. If the scaling is
  // set to say 20% then we want the resulting volume to be -21, i.e. 20% of the
  // change. Expressed as an equation:
  //   a_r = a_0 + m/M * (a_i - a_0)     - where a_0 is the current airplay volume, a_i is the input and a_r is the scaled result
  //
  // Since current volume (device->volume) is measured on the 0-100 scale, and the
  // result of this func should also be on that scale, we have the following two
  // relationships (the first is also found in _from_pct() above):
  //   a_0 = -30 + m/M * 30/100 * v_0    - where v_0 is device->volume
  //   v_r = M/m * 100 * (1 + a_r / 30)  - converts a_r to v_r which is [0-100]
  //
  // Solving these three equations gives this:
  volume_base = 100.0 * (1.0 + airplay_volume / 30.0);
  volume = (float)device->volume * (1.0 - (float)max_volume/AIRPLAY_CONFIG_MAX_VOLUME) + volume_base;

*/
  // RAOP volume: -144.0 is off, -30.0 - 0 scaled by max_volume maps to 0 - 100
  volume = (100.0 * (airplay_volume / 30.0 + 1.0) * AIRPLAY_CONFIG_MAX_VOLUME / (float)max_volume);
  return MAX(0, MIN(100, (int)volume));
}

/* Volume in [0 - 100] */
static int
airplay_set_volume_one(struct output_device *device, int callback_id)
{
  struct airplay_session *session = device->session;

  if (!session || !(session->state & AIRPLAY_STATE_F_CONNECTED))
    return 0;

  session->volume = device->volume;
  session->callback_id = callback_id;

  sequence_start(AIRPLAY_SEQ_SEND_VOLUME, session, NULL, "set_volume_one");

  return 1;
}

static void
airplay_keep_alive_timer_cb(int fd, short what, void *arg)
{
  struct airplay_session *session;

  if (!airplay_sessions)
    {
      event_del(keep_alive_timer);
      return;
    }

  for (session = airplay_sessions; session; session = session->next)
    {
      if (!(session->state & AIRPLAY_STATE_F_CONNECTED))
	continue;

      airplay_metadata_keep_alive_send(session);
    }

  evtimer_add(keep_alive_timer, &keep_alive_tv);
}


/* -------------------- Creation and sending of RTP packets  ---------------- */

static int
packet_encrypt(uint8_t **out, size_t *out_len, struct rtp_packet *pkt, struct airplay_session *session)
{
  uint8_t authtag[16];
  uint8_t nonce[12] = { 0 };
  int nonce_offset = 4;
  uint8_t *write_ptr;
  int ret;

  // Alloc so authtag and nonce can be appended
  *out_len = pkt->data_len + sizeof(authtag) + sizeof(nonce) - nonce_offset;
  *out = malloc(*out_len);
  write_ptr = *out;

  // Using seqnum as nonce not very secure, but means that when we resend
  // packets they will be identical to the original
  memcpy(nonce + nonce_offset, &pkt->seqnum, sizeof(pkt->seqnum));

  // The RTP header is not encrypted
  memcpy(write_ptr, pkt->header, pkt->header_len);
  write_ptr = *out + pkt->header_len;

  // Timestamp and SSRC are used as AAD = pkt->header + 4, len 8
  ret = chacha_encrypt(write_ptr, pkt->payload, pkt->payload_len, pkt->header + 4, 8, authtag, sizeof(authtag), nonce, sizeof(nonce), session->packet_cipher_hd);
  if (ret < 0)
    {
      free(*out);
      return -1;
    }

  write_ptr += pkt->payload_len;
  memcpy(write_ptr, authtag, sizeof(authtag));
  write_ptr += sizeof(authtag);
  memcpy(write_ptr, nonce + nonce_offset, sizeof(nonce) - nonce_offset);

  return 0;
}

static int
packet_send(struct airplay_session *session, struct rtp_packet *pkt)
{
  uint8_t *buf;
  size_t buf_len;
  ssize_t sent;
  int ret;

  if (!session)
    return -1;

  if (session->supports_encryption)
    {
      ret = packet_encrypt(&buf, &buf_len, pkt, session);
      if (ret < 0)
	return -1;

      sent = send(session->server_fd, buf, buf_len, 0);
      free(buf);
    }
  else
    {
      buf = pkt->data;
      buf_len = pkt->data_len;
      sent = send(session->server_fd, buf, buf_len, 0);
    }

  if (sent < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Send error for '%s': %s\n", session->devname, strerror(errno));

      // Can't free it right away, it would make the ->next in the calling
      // master_session and session loops invalid
      deferred_session_failure(session);
      return -1;
    }
  else if (sent != buf_len)
    {
      DPRINTF(E_WARN, L_AIRPLAY, "Partial send (%zd) for '%s'\n", sent, session->devname);
      return -1;
    }

/*
  DPRINTF(E_DBG, L_AIRPLAY, "RTP PACKET seqnum %u, rtptime %u, payload 0x%x, pktbuf_s %zu\n",
    session->master_session->rtp_session->seqnum,
    session->master_session->rtp_session->pos,
    pkt->header[1],
    session->master_session->rtp_session->pktbuf_len
    );
*/

  return 0;
}

static void
control_packet_send(struct airplay_session *session, struct rtp_packet *pkt)
{
  socklen_t addrlen;
  int fd;
  int ret;

  switch (session->family)
    {
      case AF_INET:
	session->naddr.sin.sin_port = htons(session->control_port);
	addrlen = sizeof(session->naddr.sin);
	fd = session->control_svc->socket.fd4;
	break;

      case AF_INET6:
	session->naddr.sin6.sin6_port = htons(session->control_port);
	addrlen = sizeof(session->naddr.sin6);
	fd = session->control_svc->socket.fd6;
	break;

      default:
	DPRINTF(E_WARN, L_AIRPLAY, "Unknown family %d\n", session->family);
	return;
    }

  ret = sendto(fd, pkt->data, pkt->data_len, 0, &session->naddr.sa, addrlen);
  if (ret < 0)
    DPRINTF(E_LOG, L_AIRPLAY, "Could not send playback sync to device '%s': %s\n", session->devname, strerror(errno));
}

static void
packets_resend(struct airplay_session *session, uint16_t seqnum, int len)
{
  struct rtp_session *rtp_session;
  struct rtp_packet *pkt;
  uint16_t s;
  int i;
  bool pkt_missing = false;

  rtp_session = session->master_session->rtp_session;

  DPRINTF(E_DBG, L_AIRPLAY, "Got retransmit request from '%s': seqnum %" PRIu16 " (len %d), next RTP session seqnum %" PRIu16 " (len %zu)\n",
    session->devname, seqnum, len, rtp_session->seqnum, rtp_session->pktbuf_len);

  // Seems iOS returns 80 d6 00 01 bd 0d 00 00 over control when the packet is
  // not in buffer. And also  resends over control, which my amp doesn't
  // understand, since it keeps asking.
  // Example of resend over control (bd0d=seqnum 48397):
  // 0x000000: 80 d6 00 ee 80 60 bd 0d b4 04 4e c0 00 00 00 00 .....`....N.....
  // 0x000010: 99 cf 26 e3 fa a0 ed da 57 50 82 c8 be 1b 81 9e ..&.....WP......
  // 0x000020: a4 a5 c5 3c 07 83 71 21 e2 91 89 6a 81 bb b0 d3 ...<..q!...j....
  // 0x000030: f4 0e 5c 7f 84 14 1f 0d e6 63 83 26 7d b3 95 24 ..\......c.&}..$
  // 0x000040: 0d 52 11 ef a5 fb 6f 9d 86 8e 69 28 79 0e 73 5b .R....o...i(y.s[
  // 0x000050: 46 1f 04 2d 6b 7e 7f 1d 1e a6 46 04 7b f8 e3 80 F..-k~....F.{...
  // 0x000060: 85 ff 11 4f f2 b5 de f7 cf 0f 7e 4b 55 e5 7b 25 ...O......~KU.{%
  // 0x000070: 13 9a 5f c3 4e f4 9d d1 f5 cb...

  // Note that seqnum may wrap around, so we don't use it for counting
  for (i = 0, s = seqnum; i < len; i++, s++)
    {
      pkt = rtp_packet_get(rtp_session, s);
      if (pkt)
	packet_send(session, pkt);
      else
	pkt_missing = true;
    }

  if (pkt_missing)
    DPRINTF(E_WARN, L_AIRPLAY, "Device '%s' retransmit request for seqnum %" PRIu16 " (len %d) is outside buffer range (next seqnum %" PRIu16 ", len %zu)\n",
      session->devname, seqnum, len, rtp_session->seqnum, rtp_session->pktbuf_len);
}

static int
packets_send(struct airplay_master_session *ams)
{
  struct rtp_packet *pkt;
  struct airplay_session *session;
  int len;

  len = alac_encode(ams->encoded_buffer, ams->encode_ctx, ams->rawbuf, ams->rawbuf_size, ams->samples_per_packet, &ams->quality);
  if (len < 0)
    return -1;

  pkt = rtp_packet_next(ams->rtp_session, len, ams->samples_per_packet, AIRPLAY_RTP_PAYLOADTYPE);

  evbuffer_remove(ams->encoded_buffer, pkt->payload, pkt->payload_len);

  for (session = airplay_sessions; session; session = session->next)
    {
      if (session->master_session != ams)
	continue;

      // Device just joined
      if (session->state == AIRPLAY_STATE_CONNECTED)
	{
	  pkt->header[1] |= RTP_MARKER_BIT; // Set marker bit, value becomes 0xe0
	  packet_send(session, pkt);
	  pkt->header[1] &= ~RTP_MARKER_BIT; // Clear marker bit
	}
      else if (session->state == AIRPLAY_STATE_STREAMING)
	{
	  packet_send(session, pkt);
	}
    }

  // Commits packet to retransmit buffer, and prepares the session for the next packet
  rtp_packet_commit(ams->rtp_session, pkt);

  return 0;
}

// Buffered AirPlay 2 (type 103) counterpart to packets_send(). There is no
// retransmit buffer and no rtp_packet_next/RTP header for the buffered
// transport; framing uses ams->buffered_seqnum/buffered_rtptime directly.
// Sends ONE encoded buffered frame (already extracted into buf) to every
// ready session on the ams, advancing seqnum/rtptime by exactly one frame.
// The caller splits a multi-frame encode result into individual calls, since
// one encode pass can produce more than one frame.
static int
buffered_frame_send(struct airplay_master_session *ams, uint8_t *buf, int len)
{
  struct airplay_session *session;
  bool wrote = false;
  bool gated = false;
  int ret;

  // Start of a (re)primed buffer: remember where it begins so the anchor can
  // point at it (see buffered_anchor_rtptime).
  if (ams->buffered_marker_pending)
    ams->buffered_anchor_rtptime = ams->buffered_rtptime;

  for (session = airplay_sessions; session; session = session->next)
    {
      if (session->master_session != ams || !session->buffered_mode)
	continue;

      if (session->state != AIRPLAY_STATE_CONNECTED && session->state != AIRPLAY_STATE_STREAMING)
	continue;

      // The receiver's decode chain doesn't start until it accepts a rate=1
      // anchor, and audio received before it has processed one is simply
      // discarded rather than queued - the sender's job is to hold off
      // delivering anything until the anchor is confirmed accepted, so the
      // first delivered frame's rtptime lands exactly on the anchor's
      // rtpTime. The counters are frozen below while holding.
      if (!session->anchor_confirmed)
	{
	  gated = true;
	  continue;
	}

      // A session that joined an already-anchored ams has an anchor naming a
      // slightly-future frame (see AIRPLAY_BUFFERED_JOIN_SLACK_FRAMES); hold
      // delivery until the write head reaches it so the first delivered frame
      // matches the anchor exactly. NOT counted as gated: the counters must
      // keep advancing (frozen counters would never reach the join point).
      if ((int32_t)(ams->buffered_rtptime - session->buffered_start_rtptime) < 0)
	continue;

      if (ams->buffered_rtptime == session->buffered_start_rtptime)
	DPRINTF(E_INFO, L_AIRPLAY, "Anchor point reached for '%s': delivering from seq %u, rtptime %u\n",
	        session->devname, ams->buffered_seqnum & 0x7fffff, ams->buffered_rtptime);

      ret = airplay_buffered_write(session->buffered, buf, len, ams->buffered_seqnum & 0x7fffff, ams->buffered_rtptime, true);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_AIRPLAY, "Error writing buffered frame to '%s'\n", session->devname);

	  // Can't free it right away, it would make the ->next in this loop
	  // and the calling ams loop in airplay_write() invalid
	  deferred_session_failure(session);
	  continue;
	}

      if (ams->buffered_marker_pending)
	DPRINTF(E_INFO, L_AIRPLAY, "First buffered frame to '%s': seq %u, rtptime %u, len %d\n",
	        session->devname, ams->buffered_seqnum & 0x7fffff, ams->buffered_rtptime, len);
      else if ((ams->buffered_seqnum % 250) == 0)
	DPRINTF(E_DBG, L_AIRPLAY, "Buffered stream to '%s': seq %u, rtptime %u, send queue %zu bytes\n",
	        session->devname, ams->buffered_seqnum & 0x7fffff, ams->buffered_rtptime, airplay_buffered_pending(session->buffered));

      wrote = true;
    }

  // Only a frame actually delivered consumes the marker: the counters advance
  // even while no session is ready to receive (e.g. during negotiation), and
  // the marker bit plus buffered_anchor_rtptime must land on the first frame
  // the receiver really gets
  if (wrote)
    ams->buffered_marker_pending = false;

  // While a session is waiting for its anchor to be accepted the counters
  // freeze at the anchor's rtpTime (payload_make_send_anchor anchors at the
  // write head), so the first frame delivered after acceptance carries
  // exactly the rtptime the receiver was told to wait for. The frame content
  // produced while frozen is dropped (a few tens of ms at stream start).
  // If another session on the same ams is already streaming, delivery to it
  // takes priority and the waiting session loses the frames instead.
  if (gated && !wrote)
    return 0;

  ams->buffered_seqnum++;
  ams->buffered_rtptime += ams->buffered_spf;

  return 0;
}

// Overview of rtptimes as they should be when starting a stream, and assuming
// the first rtptime (pos) is 88200:
//   sync pkt:  cur_pos = 0, rtptime = 88200
//   audio pkt: rtptime = 88200
//   RECORD:    rtptime = 88200
//   SET_PARAMETER text/artwork:
//              rtptime = 88200
//   SET_PARAMETER progress:
//              progress = 72840/~88200/[len]
static inline void
timestamp_set(struct airplay_master_session *ams, struct timespec ts)
{
  uint32_t pending_samples;

  // The last write from the player had a timestamp which has been passed to
  // this function as ts. This is the player clock, which is more precise than
  // the actual clock because it gives us a calculated time reference, which is
  // independent of how busy the thread is. We save that here, we need this for
  // reference when sending sync packets and progress.
  ams->cur_stamp.ts = ts;

  // So what rtptime should be playing, i.e. coming out of the speaker, at time
  // ts (which is normally "now")? Let's calculate by example:
  //   - we started playback with a rtptime (pos) of X
  //   - up until time ts we have received a 1000 samples from the player
  //   - ams->output_buffer_samples is configured to 400 samples
  //   -> we should be playing rtptime X + 600
  //
  // So how do we measure samples received from player? We know that from the
  // pos, which says how much has been sent to the device, and from ams->input_buffer,
  // which is the unsent stuff being buffered:
  //   - received = (pos - X) + ams->input_buffer_samples
  //
  // This means the rtptime is computed as:
  //   - rtptime = X + received - ams->output_buffer_samples
  //   -> rtptime = X + (pos - X) + ams->input_buffer_samples - ams->out_buffer_samples
  //   -> rtptime = pos + ams->input_buffer_samples - ams->output_buffer_samples
  //
  // A buffered ams has no input_buffer (unsent PCM sits in the encoder's
  // in-queue instead), so its pending count comes from there.
  pending_samples = ams->encoder ? airplay_encoder_pending_samples(ams->encoder) : ams->input_buffer_samples;

  ams->cur_stamp.pos = ams->rtp_session->pos + pending_samples - ams->output_buffer_samples;
}

static void
packets_sync_send(struct airplay_master_session *ams)
{
  struct rtp_packet *sync_pkt;
  struct rtcp_timestamp cur_stamp;
  struct airplay_session *session;
  struct timespec ts;
  bool is_sync_time;

  // Check if it is time send a sync packet to sessions that are already running
  is_sync_time = rtp_sync_is_time(ams->rtp_session);

  // Just used for logging, the clock shouldn't be too far from ams->cur_stamp.ts
  clock_gettime(CLOCK_MONOTONIC, &ts);

  for (session = airplay_sessions; session; session = session->next)
    {
      if (session->master_session != ams)
	continue;

      cur_stamp = ams->cur_stamp;

      // Apply user configured offset
      cur_stamp.pos -= session->offset_samples;

      // A device has joined and should get an init sync packet
      if (session->state == AIRPLAY_STATE_CONNECTED)
	{
	  sync_pkt = rtp_sync_packet_next(ams->rtp_session, cur_stamp, 0x90);
	  control_packet_send(session, sync_pkt);

	  DPRINTF(E_DBG, L_AIRPLAY, "Start sync packet sent to '%s': offset=%d, cur_pos=%" PRIu32 ", cur_ts=%ld.%09ld, clock=%ld.%09ld, rtptime=%" PRIu32 ", timing=%s\n",
	    session->devname, session->offset_samples, cur_stamp.pos, (long)cur_stamp.ts.tv_sec, (long)cur_stamp.ts.tv_nsec, (long)ts.tv_sec, (long)ts.tv_nsec,
	    ams->rtp_session->pos, ams->use_ptp ? "PTP" : "NTP");
	}
      else if (is_sync_time && session->state == AIRPLAY_STATE_STREAMING)
	{
	  sync_pkt = rtp_sync_packet_next(ams->rtp_session, cur_stamp, 0x80);
	  control_packet_send(session, sync_pkt);
	}
    }
}


/* ------------------------- Time and control service ----------------------- */

static void
service_stop(struct airplay_service *svc)
{
  if (svc->ev4)
    event_free(svc->ev4);
  if (svc->ev6)
    event_free(svc->ev6);

  svc->ev4 = NULL;
  svc->ev6 = NULL;

  net_socket_close(&svc->socket);

  svc->port = 0;
}

static int
service_start(struct airplay_service *svc, event_callback_fn cb, unsigned short port, const char *log_service_name)
{
  int ret;

  memset(svc, 0, sizeof(struct airplay_service));

  svc->socket.fd4 = -1;
  svc->socket.fd6 = -1;

  ret = net_bind(&svc->socket, &port, SOCK_DGRAM, log_service_name);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not start '%s' service\n", log_service_name);
      goto error;
    }

  if (svc->socket.fd4 >= 0) {
    svc->ev4 = event_new(evbase_player, svc->socket.fd4, EV_READ | EV_PERSIST, cb, svc);
    if (!svc->ev4)
      goto error;

    event_add(svc->ev4, NULL);
  }

  if (svc->socket.fd6 >= 0) {
    svc->ev6 = event_new(evbase_player, svc->socket.fd6, EV_READ | EV_PERSIST, cb, svc);
    if (!svc->ev6)
      goto error;

    event_add(svc->ev6, NULL);
  }

  svc->port = port;

  return 0;

 error:
  service_stop(svc);
  return -1;
}

static void
timing_svc_cb(int fd, short what, void *arg)
{
  union net_sockaddr peer_addr;
  socklen_t peer_addrlen = sizeof(peer_addr);
  char address[INET6_ADDRSTRLEN];
  uint8_t req[32];
  uint8_t res[32];
  struct ntp_stamp recv_stamp;
  struct ntp_stamp xmit_stamp;
  int ret;

  ret = timing_get_clock_ntp(&recv_stamp);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Couldn't get receive timestamp\n");
      return;
    }

  peer_addrlen = sizeof(peer_addr);
  ret = recvfrom(fd, req, sizeof(req), 0, &peer_addr.sa, &peer_addrlen);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Error reading timing request: %s\n", strerror(errno));
      return;
    }

  if (ret != 32)
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_WARN, L_AIRPLAY, "Got timing request from %s with size %d\n", address, ret);
      return;
    }

  if ((req[0] != 0x80) || (req[1] != 0xd2))
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_WARN, L_AIRPLAY, "Packet header from %s doesn't match timing request (got 0x%02x%02x, expected 0x80d2)\n", address, req[0], req[1]);
      return;
    }

  memset(res, 0, sizeof(res));

  /* Header */
  res[0] = 0x80;
  res[1] = 0xd3;
  res[2] = req[2];

  /* Copy client timestamp */
  memcpy(res + 8, req + 24, 8);

  /* Receive timestamp */
  recv_stamp.sec = htobe32(recv_stamp.sec);
  recv_stamp.frac = htobe32(recv_stamp.frac);
  memcpy(res + 16, &recv_stamp.sec, 4);
  memcpy(res + 20, &recv_stamp.frac, 4);

  /* Transmit timestamp */
  ret = timing_get_clock_ntp(&xmit_stamp);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Couldn't get transmit timestamp, falling back to receive timestamp\n");

      /* Still better than failing altogether
       * recv/xmit are close enough that it shouldn't matter much
       */
      memcpy(res + 24, &recv_stamp.sec, 4);
      memcpy(res + 28, &recv_stamp.frac, 4);
    }
  else
    {
      xmit_stamp.sec = htobe32(xmit_stamp.sec);
      xmit_stamp.frac = htobe32(xmit_stamp.frac);
      memcpy(res + 24, &xmit_stamp.sec, 4);
      memcpy(res + 28, &xmit_stamp.frac, 4);
    }

  ret = sendto(fd, res, sizeof(res), 0, &peer_addr.sa, peer_addrlen);
  if (ret < 0)
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_LOG, L_AIRPLAY, "Could not send timing reply to %s: %s\n", address, strerror(errno));
      return;
    }
}

static void
control_svc_cb(int fd, short what, void *arg)
{
  union net_sockaddr peer_addr = { 0 };
  socklen_t peer_addrlen = sizeof(peer_addr);
  char address[INET6_ADDRSTRLEN];
  struct airplay_session *session;
  uint8_t req[8];
  uint16_t seq_start;
  uint16_t seq_len;
  int ret;

  ret = recvfrom(fd, req, sizeof(req), 0, &peer_addr.sa, &peer_addrlen);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Error reading control request: %s\n", strerror(errno));
      return;
    }

  if (ret != 8)
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_WARN, L_AIRPLAY, "Got control request from %s with size %d\n", address, ret);
      return;
    }

  if ((req[0] != 0x80) || (req[1] != 0xd5))
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_WARN, L_AIRPLAY, "Packet header from %s doesn't match retransmit request (got 0x%02x%02x, expected 0x80d5)\n", address, req[0], req[1]);
      return;
    }

  session = session_find_by_address(&peer_addr);
  if (!session)
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_WARN, L_AIRPLAY, "Control request from %s; not a AirPlay client\n", address);
      return;
    }

  memcpy(&seq_start, req + 4, 2);
  memcpy(&seq_len, req + 6, 2);

  seq_start = be16toh(seq_start);
  seq_len = be16toh(seq_len);

  packets_resend(session, seq_start, seq_len);
}


/* -------------------- Handlers for sending RTSP requests ------------------ */

static int
payload_make_flush(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  struct airplay_master_session *ams = session->master_session;
  char buf[64];
  int ret;

  /* Restart sequence */
  ret = snprintf(buf, sizeof(buf), "seq=%" PRIu16 ";rtptime=%u", ams->rtp_session->seqnum, ams->rtp_session->pos);
  if ((ret < 0) || (ret >= sizeof(buf)))
    {
      DPRINTF(E_LOG, L_AIRPLAY, "RTP-Info too big for buffer in FLUSH request\n");
      return -1;
    }
  evrtsp_add_header(req->output_headers, "RTP-Info", buf);

  return 0;
}

// Buffered AirPlay 2 (type 103) counterpart to FLUSH. Invalidates the
// receiver's current anchor (see response_handler_flush_buffered): playback
// resumes at a new wall time, so a fresh SETRATEANCHORTIME must follow before
// any more audio is written.
static int
payload_make_flush_buffered(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  struct airplay_master_session *ams = session->master_session;
  plist_t root;
  uint8_t *data;
  size_t len;
  uint32_t flush_until_seq;
  int ret;

  flush_until_seq = ams->buffered_seqnum & 0x7fffff;

  root = plist_new_dict();
  wplist_dict_add_uint(root, "flushUntilSeq", flush_until_seq);
  wplist_dict_add_uint(root, "flushUntilTS", ams->buffered_rtptime);

  DPRINTF(E_DBG, L_AIRPLAY, "FLUSHBUFFERED payload for '%s': flushUntilSeq=%u, flushUntilTS=%u\n",
          session->devname, flush_until_seq, ams->buffered_rtptime);

  ret = wplist_to_bin(&data, &len, root);
  plist_free(root);

  if (ret < 0)
    return -1;

  evbuffer_add(req->output_buffer, data, len);

  return 0;
}

// Buffered receivers expect the MediaRemote command table and audio-mode POST
// before rendering: an empty updateMRSupportedCommands and a POST /audioMode
// {audioMode: "default"} bracket SETUP(stream). Buffered mode only - skipped
// otherwise.
static int
payload_make_command_supported_commands(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  plist_t root;
  plist_t params;
  uint8_t *data;
  size_t len;
  int ret;

  if (!session->buffered_mode)
    return 1; // Skip this request in the sequence

  params = plist_new_dict();
  plist_dict_set_item(params, "mrSupportedCommandsFromSender", plist_new_array());

  root = plist_new_dict();
  wplist_dict_add_string(root, "type", "updateMRSupportedCommands");
  plist_dict_set_item(root, "params", params);

  ret = wplist_to_bin(&data, &len, root);
  plist_free(root);

  if (ret < 0)
    return -1;

  evbuffer_add(req->output_buffer, data, len);

  return 0;
}

// The full MediaRemote command table, sent right after the empty one above.
// mrSupportedCommandsFromSender is an array of serialized CommandInfo binary
// plists: {kCommandInfoCommandKey: <MRMediaRemoteCommand id>,
// kCommandInfoEnabledKey: true}. A MediaRemote-centric receiver may gate
// rendering on the sender publishing a usable command table. The per-command
// options are omitted, which is still a valid CommandInfo. Buffered mode only.
static int
payload_make_command_supported_commands_full(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  static const uint32_t command_ids[] = {
      0x61bd, 0x61bc, 0x95, 0x87, 0x86, 0x84, 0x83, 0x82, 0x81, 0x80, 0x7f,
      0x7d, 0x7a, 0x1a, 0x19, 0x18, 0x16, 0x15, 0x13, 0x12, 0x11, 0x0a, 0x0b,
      0x08, 0x09, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
  };
  plist_t root;
  plist_t params;
  plist_t commands;
  plist_t cmdinfo;
  uint8_t *blob;
  size_t blob_len;
  uint8_t *data;
  size_t len;
  int i;
  int ret;

  if (!session->buffered_mode)
    return 1; // Skip this request in the sequence

  commands = plist_new_array();
  for (i = 0; i < ARRAY_SIZE(command_ids); i++)
    {
      cmdinfo = plist_new_dict();
      wplist_dict_add_uint(cmdinfo, "kCommandInfoCommandKey", command_ids[i]);
      wplist_dict_add_bool(cmdinfo, "kCommandInfoEnabledKey", true);

      ret = wplist_to_bin(&blob, &blob_len, cmdinfo);
      plist_free(cmdinfo);
      if (ret < 0)
	{
	  plist_free(commands);
	  return -1;
	}

      plist_array_append_item(commands, plist_new_data((const char *)blob, blob_len));
      free(blob);
    }

  params = plist_new_dict();
  plist_dict_set_item(params, "mrSupportedCommandsFromSender", commands);

  root = plist_new_dict();
  wplist_dict_add_string(root, "type", "updateMRSupportedCommands");
  plist_dict_set_item(root, "params", params);

  ret = wplist_to_bin(&data, &len, root);
  plist_free(root);

  if (ret < 0)
    return -1;

  evbuffer_add(req->output_buffer, data, len);

  return 0;
}

// POST /audioMode after SETUP(stream). Buffered receivers expect this before
// rendering. Buffered mode only - skipped otherwise.
static int
payload_make_audio_mode(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  plist_t root;
  uint8_t *data;
  size_t len;
  int ret;

  if (!session->buffered_mode)
    return 1; // Skip this request in the sequence

  root = plist_new_dict();
  wplist_dict_add_string(root, "audioMode", "default");

  ret = wplist_to_bin(&data, &len, root);
  plist_free(root);

  if (ret < 0)
    return -1;

  evbuffer_add(req->output_buffer, data, len);

  return 0;
}

// The receiver's decode chain doesn't start until it gets a rate=1 anchor -
// audio queued before that point is simply held, so it is fine to start
// writing buffered packets right after SETUP without waiting for this to
// complete. Timescale is CLOCK_MONOTONIC (see ptpd_network_time_get()). The
// first anchor on a master session points a lead time into the future (the
// start_buffer_ms setting - this becomes the receiver's cushion) and stores
// the (rtpTime, networkTime) point in the ams; anchors for sessions joining
// the same ams later are derived from that stored point so all receivers
// share one rtptime->networkTime mapping (multi-room sync).
static int
payload_make_send_anchor(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  struct airplay_master_session *ams = session->master_session;
  plist_t root;
  uint8_t *data;
  size_t len;
  uint64_t secs;
  uint64_t frac;
  uint64_t frac_before;
  uint32_t anchor_rtptime;
  uint64_t lead_ms;
  uint32_t delta;
  int ret;

  if (ams->buffered_anchor_mapped)
    {
      // A session joining an already-anchored master session. Its counters
      // are NOT frozen (another session is live), so anchoring at the
      // current write head would name a frame this session never gets
      // delivered - the head moves on while the anchor is in flight. Name a
      // slightly-future frame instead and hold delivery until the write head
      // reaches it (see the buffered_start_rtptime gate in
      // buffered_frame_send), so the first delivered frame matches exactly.
      anchor_rtptime = ams->buffered_rtptime + AIRPLAY_BUFFERED_JOIN_SLACK_FRAMES * ams->buffered_spf;

      // Extend the stored mapping to the anchor frame: rtptime advances at
      // AIRPLAY_BUFFERED_SAMPLE_RATE per networkTime second (rate=1).
      // 384307168202282 is floor(2^64 / 48000), i.e. frac units per sample;
      // detect carry into secs via unsigned overflow.
      delta = anchor_rtptime - ams->buffered_anchor_map_rtptime;
      secs = ams->buffered_anchor_map_secs + delta / AIRPLAY_BUFFERED_SAMPLE_RATE;
      frac_before = ams->buffered_anchor_map_frac;
      frac = frac_before + (uint64_t)(delta % AIRPLAY_BUFFERED_SAMPLE_RATE) * (uint64_t)384307168202282ULL;
      if (frac < frac_before)
	secs++;
    }
  else
    {
      ret = ptpd_network_time_get(&secs, &frac);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_AIRPLAY, "Could not read network time for anchor to '%s'\n", session->devname);
	  return -1;
	}

      // First anchor on this ams: the counters are frozen until it is
      // accepted, so the write head is exactly the first delivered frame
      anchor_rtptime = ams->buffered_rtptime;

      // Add the lead. frac is a 2^64 fixed-point fraction of a second;
      // 18446744073709551 is floor(2^64 / 1000), i.e. frac units per 1ms.
      // Whole seconds go into secs directly - the ms-to-frac product
      // overflows uint64 for lead_ms >= 1000. Detect carry via unsigned
      // overflow.
      lead_ms = outputs_buffer_duration_ms_get();
      secs += lead_ms / 1000;
      frac_before = frac;
      frac += (lead_ms % 1000) * (uint64_t)18446744073709551ULL;
      if (frac < frac_before)
	secs++;

      ams->buffered_anchor_mapped = true;
      ams->buffered_anchor_map_rtptime = anchor_rtptime;
      ams->buffered_anchor_map_secs = secs;
      ams->buffered_anchor_map_frac = frac;
    }

  session->buffered_start_rtptime = anchor_rtptime;

  root = plist_new_dict();
  wplist_dict_add_uint(root, "networkTimeSecs", secs);
  // frac and the timeline ID can exceed INT64_MAX (the timeline ID always
  // does - libairptp's clock ids start 0xFFFF). plist_new_uint() would then
  // emit them as 16-byte bplist integers; these fields must be serialized as
  // 8-byte signed ints or some receivers reject the anchor. Force the 8-byte
  // encoding via the signed-int path; the receiver reads the bits back as
  // uint64.
  wplist_dict_add_int(root, "networkTimeFrac", (int64_t)frac);
  wplist_dict_add_int(root, "networkTimeTimelineID", (int64_t)ptpd_clock_id_get());
  wplist_dict_add_uint(root, "networkTimeFlags", 0);
  // The anchor names exactly the first frame this session will be delivered
  // (see anchor_rtptime above) - the strictest reading of the anchor-then-data
  // ordering, and safe against receivers discarding audio received before the
  // anchor or stalling on a missing anchor frame.
  wplist_dict_add_uint(root, "rtpTime", anchor_rtptime);
  wplist_dict_add_uint(root, "rate", 1);

  DPRINTF(E_DBG, L_AIRPLAY, "SETRATEANCHORTIME payload for '%s': networkTimeSecs=%" PRIu64 ", networkTimeFrac=%" PRIu64 ", networkTimeTimelineID=%" PRIu64 ", rtpTime=%u (%s), rate=1\n",
          session->devname, secs, frac, ptpd_clock_id_get(), anchor_rtptime,
          anchor_rtptime == session->master_session->buffered_rtptime ? "write head" : "join point");

  ret = wplist_to_bin(&data, &len, root);
  plist_free(root);

  if (ret < 0)
    return -1;

  evbuffer_add(req->output_buffer, data, len);

  return 0;
}

// Sending an empty plist seems to mean close connection. At least according to
// shairport-sync handle_teardown_2(). iOS seems to send first a teardown with
// stream (to close that), then with an empty plist.
static int
payload_make_teardown(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  plist_t root;
  uint8_t *data;
  size_t len;
  int ret;

  // Normally we update status when we get the response, but teardown is an
  // exception because we want to stop writing to the device immediately
  session->state = AIRPLAY_STATE_TEARDOWN;

  // stream = plist_new_dict();
  // wplist_dict_add_uint(stream, "streamID", 0); // Do we have a stream ID?
  // wplist_dict_add_uint(stream, "type", AIRPLAY_RTP_PAYLOADTYPE);
  // streams = plist_new_array();
  // plist_array_append_item(streams, stream);

  root = plist_new_dict();
  // plist_dict_set_item(root, "streams", streams);

  ret = wplist_to_bin(&data, &len, root);
  plist_free(root);

  if (ret < 0)
    return -1;

  evbuffer_add(req->output_buffer, data, len);

  return 0;
}

static int
payload_make_set_volume(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  float raop_volume;
  char volstr[32];
  int ret;

  raop_volume = airplay_volume_from_pct(session->volume, session->devname);

  /* Don't let locales get in the way here */
  /* We use -%d and -(int)raop_volume so -0.3 won't become 0.3 */
  snprintf(volstr, sizeof(volstr), "-%d.%06d", -(int)raop_volume, -(int)(1000000.0 * (raop_volume - (int)raop_volume)));

  DPRINTF(E_DBG, L_AIRPLAY, "Sending volume %s to '%s'\n", volstr, session->devname);

  ret = evbuffer_add_printf(req->output_buffer, "volume: %s\r\n", volstr);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Out of memory for SET_PARAMETER payload (volume)\n");
      return -1;
    }

  return 0;
}

static int
payload_make_send_progress(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  struct output_metadata *metadata = arg;
  uint32_t start;
  uint32_t display;
  uint32_t pos;
  uint32_t end;
  int ret;

  metadata_rtptimes_get(&start, &display, &pos, &end, session->master_session, metadata);

  ret = evbuffer_add_printf(req->output_buffer, "progress: %u/%u/%u\r\n", display, pos, end);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not build progress string for sending\n");
      return -1;
    }

  ret = rtpinfo_header_add(req, session, metadata);
  if (ret < 0)
    return -1;

  return 0;
}

static int
payload_make_send_artwork(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  struct output_metadata *metadata = arg;
  struct airplay_metadata *rmd = metadata->priv;
  char *ctype;
  uint8_t *buf;
  size_t len;
  int ret;

  switch (rmd->artwork_fmt)
    {
      case ART_FMT_PNG:
	ctype = "image/png";
	break;

      case ART_FMT_JPEG:
	ctype = "image/jpeg";
	break;

      default:
	DPRINTF(E_LOG, L_AIRPLAY, "Unsupported artwork format %d\n", rmd->artwork_fmt);
	return -1;
    }

  buf = evbuffer_pullup(rmd->artwork, -1);
  len = evbuffer_get_length(rmd->artwork);

  ret = evbuffer_add(req->output_buffer, buf, len);
  if (ret != 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not copy artwork for sending\n");
      return -1;
    }

  ret = rtpinfo_header_add(req, session, metadata);
  if (ret < 0)
    return -1;

  evrtsp_add_header(req->output_headers, "Content-Type", ctype);

  return 0;
}

static int
payload_make_send_text(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  struct output_metadata *metadata = arg;
  struct airplay_metadata *rmd = metadata->priv;
  uint8_t *buf;
  size_t len;
  int ret;

  buf = evbuffer_pullup(rmd->metadata, -1);
  len = evbuffer_get_length(rmd->metadata);

  ret = evbuffer_add(req->output_buffer, buf, len);
  if (ret != 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not copy metadata for sending\n");
      return -1;
    }

  ret = rtpinfo_header_add(req, session, metadata);
  if (ret < 0)
    return -1;

  return 0;
}


/*
Audio formats

Bit 	Value 	Type
2 	0x4 	PCM/8000/16/1
3 	0x8 	PCM/8000/16/2
4 	0x10 	PCM/16000/16/1
5 	0x20 	PCM/16000/16/2
6 	0x40 	PCM/24000/16/1
7 	0x80 	PCM/24000/16/2
8 	0x100 	PCM/32000/16/1
9 	0x200 	PCM/32000/16/2
10 	0x400 	PCM/44100/16/1
11 	0x800 	PCM/44100/16/2
12 	0x1000 	PCM/44100/24/1
13 	0x2000 	PCM/44100/24/2
14 	0x4000 	PCM/48000/16/1
15 	0x8000 	PCM/48000/16/2
16 	0x10000 	PCM/48000/24/1
17 	0x20000 	PCM/48000/24/2
18 	0x40000 	ALAC/44100/16/2
19 	0x80000 	ALAC/44100/24/2
20 	0x100000 	ALAC/48000/16/2
21 	0x200000 	ALAC/48000/24/2
22 	0x400000 	AAC-LC/44100/2
23 	0x800000 	AAC-LC/48000/2
24 	0x1000000 	AAC-ELD/44100/2
25 	0x2000000 	AAC-ELD/48000/2
26 	0x4000000 	AAC-ELD/16000/1
27 	0x8000000 	AAC-ELD/24000/1
28 	0x10000000 	OPUS/16000/1
29 	0x20000000 	OPUS/24000/1
30 	0x40000000 	OPUS/48000/1
31 	0x80000000 	AAC-ELD/44100/1
32 	0x100000000 	AAC-ELD/48000/1
*/
static int
payload_make_setup_stream(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  plist_t root;
  plist_t streams;
  plist_t stream;
  uint8_t *data;
  size_t len;
  int ret;

  stream = plist_new_dict();

  if (session->buffered_mode)
    {
      // Compression type: 1 LPCM, 2 ALAC, 4 AAC-LC, 8 AAC-ELD. The ALAC 48k/24
      // profile (format 21) is ct=2, same as the realtime ALAC stream below;
      // the AAC-LC stereo profile (format 23) is ct=4.
      unsigned int buffered_ct = (session->buffered_format_id == AIRPLAY_FORMAT_ID_ALAC48K_24_STEREO) ? 2 : 4;

      wplist_dict_add_uint(stream, "audioFormat", AIRPLAY_BUFFERED_AUDIOFORMAT(session->buffered_format_id)); // 1 << format id, see comment on the #define
      wplist_dict_add_uint(stream, "audioFormatIndex", session->buffered_format_id);
      wplist_dict_add_string(stream, "audioMode", "default");
      wplist_dict_add_uint(stream, "ct", buffered_ct);
      wplist_dict_add_data(stream, "shk", session->shared_secret, AIRPLAY_AUDIO_KEY_LEN);
      wplist_dict_add_uint(stream, "spf", session->master_session->buffered_spf); // frames per packet
      wplist_dict_add_uint(stream, "type", AIRPLAY_BUFFERED_RTP_TYPE); // RTP type, 0x60 = 96 real time, 0x67 = 103 buffered

      // streamConnections tells the receiver how to key the buffered data
      // connection; streamConnectionKeyUseStreamEncryptionKey=true means "use
      // the shk key already handed over above" rather than some other key
      // exchange. Some receivers require this to be present even though the
      // effect is the same as the implicit default.
      plist_t stream_connections = plist_new_dict();
      plist_t stream_connection_rtp = plist_new_dict();
      wplist_dict_add_bool(stream_connection_rtp, "streamConnectionKeyUseStreamEncryptionKey", true);
      plist_dict_set_item(stream_connections, "streamConnectionTypeRTP", stream_connection_rtp);
      plist_dict_set_item(stream, "streamConnections", stream_connections);

      wplist_dict_add_bool(stream, "supportsDynamicStreamID", true);
      wplist_dict_add_string(stream, "clientID", "com.apple.Music");
      // A full signed 64-bit value is expected here, not the 32-bit session id
      wplist_dict_add_int(stream, "streamConnectionID", session->stream_connection_id);

      DPRINTF(E_DBG, L_AIRPLAY,
              "SETUP(stream) payload for '%s': audioFormat=%" PRIu64 ", audioFormatIndex=%u, ct=%u, spf=%u, type=%u (buffered), supportsDynamicStreamID=1, clientID=com.apple.Music, streamConnectionID=%" PRId64 ", streamConnections=RTP/useStreamEncryptionKey\n",
              session->devname, (uint64_t)AIRPLAY_BUFFERED_AUDIOFORMAT(session->buffered_format_id), (unsigned)session->buffered_format_id,
              buffered_ct, (unsigned)session->master_session->buffered_spf,
              (unsigned)AIRPLAY_BUFFERED_RTP_TYPE, session->stream_connection_id);
    }
  else
    {
      wplist_dict_add_uint(stream, "audioFormat", 262144); // 0x40000 ALAC/44100/16/2
      wplist_dict_add_string(stream, "audioMode", "default");
      wplist_dict_add_uint(stream, "controlPort", session->control_svc->port);
      wplist_dict_add_uint(stream, "ct", 2); // Compression type, 1 LPCM, 2 ALAC, 4 AAC-LC, 8 AAC-ELD
      wplist_dict_add_bool(stream, "isMedia", true); // ?
      wplist_dict_add_uint(stream, "latencyMax", 88200); // TODO how do these latencys work?
      wplist_dict_add_uint(stream, "latencyMin", 11025); // AIRPLAY_AUDIO_LATENCY_MS in samples, see comment in rtp_sync_packet_next()
      wplist_dict_add_data(stream, "shk", session->shared_secret, AIRPLAY_AUDIO_KEY_LEN);
      wplist_dict_add_uint(stream, "spf", AIRPLAY_SAMPLES_PER_PACKET); // frames per packet
      wplist_dict_add_uint(stream, "sr", AIRPLAY_QUALITY_SAMPLE_RATE_DEFAULT); // sample rate
      wplist_dict_add_uint(stream, "type", AIRPLAY_RTP_PAYLOADTYPE); // RTP type, 0x60 = 96 real time, 103 buffered
      wplist_dict_add_bool(stream, "supportsDynamicStreamID", false);
      wplist_dict_add_uint(stream, "streamConnectionID", session->session_id); // Hopefully fine since we have one stream per session

      DPRINTF(E_DBG, L_AIRPLAY,
              "SETUP(stream) payload for '%s': audioFormat=%u, controlPort=%u, latencyMin=%u, latencyMax=%u, spf=%u, sr=%u, type=%u (realtime), supportsDynamicStreamID=%u, streamConnectionID=%u\n",
              session->devname, 262144U, session->control_svc->port, 11025U, 88200U,
              AIRPLAY_SAMPLES_PER_PACKET, AIRPLAY_QUALITY_SAMPLE_RATE_DEFAULT,
              AIRPLAY_RTP_PAYLOADTYPE, 0U, session->session_id);
    }

  streams = plist_new_array();
  plist_array_append_item(streams, stream);

  root = plist_new_dict();
  plist_dict_set_item(root, "streams", streams);
  ret = wplist_to_bin(&data, &len, root);
  plist_free(root);

  if (ret < 0)
    return -1;

  evbuffer_add(req->output_buffer, data, len);

  return 0;
}

// Mysterious empty request, but iOS sends it
static int
payload_make_record(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  return 0;
}

// Appends the receiver addresses of the OTHER members of this session's stereo
// pair to the SETPEERS array. Each member of a pair must know about its
// sibling so the two speakers form a PTP mesh and phase-lock to each other
// rather than only to our clock, which keeps their stereo image aligned.
// owntone drives a pair as one session per member (both live in
// airplay_sessions at once); siblings are matched by shared normalized device
// group id, which is equal for both members and NULL for ungrouped devices, so
// the ATV and solo devices are unaffected. Returns the number appended.
static int
setpeers_add_group_members(plist_t array, struct airplay_session *session)
{
  struct output_device *device;
  struct output_device *other;
  struct airplay_session *s;
  const char *group_id;
  const char *other_group_id;
  int count = 0;

  device = outputs_device_get(session->device_id);
  if (!device)
    return 0;

  group_id = outputs_device_group_id(device);
  if (!group_id)
    return 0; // Not part of a stereo pair

  for (s = airplay_sessions; s; s = s->next)
    {
      if (s == session || !s->address)
	continue;

      other = outputs_device_get(s->device_id);
      if (!other)
	continue;

      other_group_id = outputs_device_group_id(other);
      if (!other_group_id || strcmp(other_group_id, group_id) != 0)
	continue; // Different pair, or not grouped

      plist_array_append_item(array, plist_new_string(s->address));
      count++;
    }

  return count;
}

static int
payload_make_setpeers(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  uint8_t *data;
  size_t len;
  int ret;
  int peers;

  if (!session->master_session->use_ptp)
    return 1; // Skip to next request

  plist_t root;

  root = plist_new_array();

  // SETPEERS lists our own addresses, so the receiver can pick a PTP path
  // back to us; it does not include the receiver's own address.
  if (session->local_v4_address)
    plist_array_append_item(root, plist_new_string(session->local_v4_address));
  if (session->local_v6_address)
    plist_array_append_item(root, plist_new_string(session->local_v6_address));

  // On a stereo pair, also list the other member(s) so the two speakers form
  // a PTP mesh and phase-align to each other, not only to us.
  peers = setpeers_add_group_members(root, session);
  if (peers > 0)
    DPRINTF(E_INFO, L_AIRPLAY, "SETPEERS for '%s': added %d stereo-pair peer address(es) for cross-clock sync\n",
            session->devname, peers);

  ret = wplist_to_bin(&data, &len, root);
  plist_free(root);

  if (ret < 0)
    return -1;

  evbuffer_add(req->output_buffer, data, len);

  return 0;
}

static int
payload_make_setup_session_ntp(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  plist_t root;
  char device_id_colon[24];
  uint8_t *data;
  size_t len;
  int ret;

  device_id_colon_make(device_id_colon, sizeof(device_id_colon), airplay_device_id);

  root = plist_new_dict();
  wplist_dict_add_string(root, "deviceID", device_id_colon);
  wplist_dict_add_string(root, "sessionUUID", session->session_uuid);
  wplist_dict_add_uint(root, "timingPort", session->timing_svc->port);
  wplist_dict_add_string(root, "timingProtocol", "NTP"); // If set to "None" then an ATV4 will not respond to stream SETUP request

  DPRINTF(E_DBG, L_AIRPLAY,
          "SETUP(session/NTP) payload for '%s': deviceID=%s, sessionUUID=%s, timingPort=%u, timingProtocol=NTP\n",
          session->devname, device_id_colon, session->session_uuid, session->timing_svc->port);

  ret = wplist_to_bin(&data, &len, root);
  plist_free(root);

  if (ret < 0)
    return -1;

  evbuffer_add(req->output_buffer, data, len);

  return 0;
}

static int
payload_make_setup_session_ptp(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  plist_t root;
  plist_t addresses;
  plist_t timingpeerinfo;
  plist_t timingpeerlist;
  char device_id_colon[24];
  uint8_t *data;
  size_t len;
  int ret;

  device_id_colon_make(device_id_colon, sizeof(device_id_colon), airplay_device_id);

  addresses = plist_new_array();
  if (session->local_v4_address)
    plist_array_append_item(addresses, plist_new_string(session->local_v4_address));
  if (session->local_v6_address)
    plist_array_append_item(addresses, plist_new_string(session->local_v6_address));

  timingpeerinfo = plist_new_dict();
  wplist_dict_add_string(timingpeerinfo, "ID", airplay_ptp_clock_uuid); // iOS sends a UUID, but where does it come from?
  wplist_dict_add_uint(timingpeerinfo, "DeviceType", 0);
  wplist_dict_add_int(timingpeerinfo, "ClockID", (int64_t)ptpd_clock_id_get()); // ClockID in plist is signed, so e.g. 0xf842885f71750008 -> -557733460333756408
  // iOS sends true; only set for buffered sessions to keep the realtime
  // path byte-identical to the existing behavior.
  wplist_dict_add_bool(timingpeerinfo, "SupportsClockPortMatchingOverride", session->buffered_mode);
  plist_dict_set_item(timingpeerinfo, "Addresses", addresses);

  timingpeerlist = plist_new_array();
  plist_array_append_item(timingpeerlist, plist_copy(timingpeerinfo));

  root = plist_new_dict();
  wplist_dict_add_string(root, "name", airplay_client_name);
  wplist_dict_add_string(root, "deviceID", device_id_colon);
  wplist_dict_add_string(root, "sessionUUID", session->session_uuid);
  wplist_dict_add_string(root, "timingProtocol", "PTP"); // If set to "None" then an ATV4 will not respond to stream SETUP request
  wplist_dict_add_string(root, "macAddress", session->local_mac_address);

  wplist_dict_add_string(root, "groupUUID", session->group_uuid);
  wplist_dict_add_bool(root, "groupContainsGroupLeader", session->group_contains_leader); // Always false: group_uuid is always a generated per-session UUID

  plist_dict_set_item(root, "timingPeerInfo", timingpeerinfo);
  plist_dict_set_item(root, "timingPeerList", timingpeerlist);

  DPRINTF(E_DBG, L_AIRPLAY,
          "SETUP(session/PTP) payload for '%s': deviceID=%s, sessionUUID=%s, timingProtocol=PTP, macAddress=%s, groupUUID=%s, groupContainsGroupLeader=%u\n",
          session->devname, device_id_colon, session->session_uuid,
          session->local_mac_address, session->group_uuid, (unsigned)session->group_contains_leader);

  ret = wplist_to_bin(&data, &len, root);
  plist_free(root);

  if (ret < 0)
    return -1;

  evbuffer_add(req->output_buffer, data, len);

  return 0;
}

static int
payload_make_setup_session(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  if (!session->master_session->use_ptp)
    return payload_make_setup_session_ntp(req, session, arg);

  return payload_make_setup_session_ptp(req, session, arg);
}

/*
The purpose of auth-setup is to authenticate the device and to exchange keys
for encryption. We don't do that, but some AirPlay 2 speakers (Sonos beam,
Airport Express fw 7.8) require this step anyway, otherwise we get a 403 to
our ANNOUNCE. So we do it with a flag for no encryption, and without actually
authenticating the device.

Good to know (source Apple's MFi Accessory Interface Specification):
- Curve25519 Elliptic-Curve Diffie-Hellman technology for key exchange
- RSA for signing and verifying and AES-128 in counter mode for encryption
- We start by sending a Curve25519 public key + no encryption flag
- The device responds with public key, MFi certificate and a signature, which
  is created by the device signing the two public keys with its RSA private
  key and then encrypting the result with the AES master key derived from the
  Curve25519 shared secret (generated from device private key and our public
  key)
- The AES key derived from the Curve25519 shared secret can then be used to
  encrypt future content
- New keys should be generated for each authentication attempt, but we don't
  do that because we don't really use this + it adds a libsodium dependency

Since we don't do auth nor encryption, we currently just ignore the reponse.
*/

#if AIRPLAY_USE_AUTH_SETUP
static int
payload_make_auth_setup(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  if (!session->supports_auth_setup)
    return 1; // skip this request

  // Flag for no encryption. 0x10 may mean encryption.
  evbuffer_add(req->output_buffer, "\x01", 1);

  evbuffer_add(req->output_buffer, airplay_auth_setup_pubkey, sizeof(airplay_auth_setup_pubkey) - 1);

  return 0;
}
#endif

/* airplay2-receiver says this about X-Apple-HKP:
 Values 0,2,3,4,6 seen.
 0 = Unauth. When Ft48TransientPairing and Ft43SystemPairing are absent
 2 = (pair-setup complete, pair-verify starts)
 3 = SystemPairing (with Ft43SystemPairing)
 4 = Transient
 6 = HomeKit
 7 = HomeKit (administration)
 */
static int
payload_make_pin_start(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  DPRINTF(E_LOG, L_AIRPLAY, "Starting device pairing for '%s', go to the web interface and enter PIN\n", session->devname);

  if (session->pair_type == PAIR_CLIENT_HOMEKIT_NORMAL)
    evrtsp_add_header(req->output_headers, "X-Apple-HKP", "3");
  else if (session->pair_type == PAIR_CLIENT_HOMEKIT_TRANSIENT)
    evrtsp_add_header(req->output_headers, "X-Apple-HKP", "4");

  return 0;
}

static int
payload_make_pair_generic(int step, struct evrtsp_request *req, struct airplay_session *session)
{
  uint8_t *body;
  size_t len;
  const char *errmsg;

  switch (step)
    {
      case 1:
	body    = pair_setup_request1(&len, session->pair_setup_ctx);
	errmsg  = pair_setup_errmsg(session->pair_setup_ctx);
	break;
      case 2:
	body    = pair_setup_request2(&len, session->pair_setup_ctx);
	errmsg  = pair_setup_errmsg(session->pair_setup_ctx);
	break;
      case 3:
	body    = pair_setup_request3(&len, session->pair_setup_ctx);
	errmsg  = pair_setup_errmsg(session->pair_setup_ctx);
	break;
      case 4:
	body    = pair_verify_request1(&len, session->pair_verify_ctx);
	errmsg  = pair_verify_errmsg(session->pair_verify_ctx);
	break;
      case 5:
	body    = pair_verify_request2(&len, session->pair_verify_ctx);
	errmsg  = pair_verify_errmsg(session->pair_verify_ctx);
	break;
      default:
	body    = NULL;
	errmsg  = "Bug! Bad step number";
    }

  if (!body)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Verification step %d request error: %s\n", step, errmsg);
      return -1;
    }

  evbuffer_add(req->output_buffer, body, len);
  free(body);

  // Required!!
  if (session->pair_type == PAIR_CLIENT_HOMEKIT_NORMAL)
    evrtsp_add_header(req->output_headers, "X-Apple-HKP", "3");
  else if (session->pair_type == PAIR_CLIENT_HOMEKIT_TRANSIENT)
    evrtsp_add_header(req->output_headers, "X-Apple-HKP", "4");

  return 0;
}

static int
payload_make_pair_setup1(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  const char *pin = arg;
  char device_id_hex[16 + 1];

  if (!pin && session->password)
    pin = session->password; // For password based authentication

  if (pin)
    session->pair_type = PAIR_CLIENT_HOMEKIT_NORMAL;

  snprintf(device_id_hex, sizeof(device_id_hex), "%016" PRIX64, airplay_device_id);

  session->pair_setup_ctx = pair_setup_new(session->pair_type, pin, NULL, NULL, device_id_hex);
  if (!session->pair_setup_ctx)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Out of memory for verification setup context\n");
      return -1;
    }

  // Only PIN/password pairing means the user has something to enter. Transient
  // pairing must not enter AUTH state: HomePods sporadically reject transient
  // pair-setup right after a TEARDOWN, and if the session is in AUTH state that
  // soft failure is reported as OUTPUT_STATE_PASSWORD, making clients prompt
  // for a PIN that does not exist.
  if (session->pair_type == PAIR_CLIENT_HOMEKIT_NORMAL)
    session->state = AIRPLAY_STATE_AUTH;

  return payload_make_pair_generic(1, req, session);
}

static int
payload_make_pair_setup2(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  return payload_make_pair_generic(2, req, session);
}

static int
payload_make_pair_setup3(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  return payload_make_pair_generic(3, req, session);
}

static int
payload_make_pair_verify1(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  struct output_device *device;
  char device_id_hex[16 + 1];

  device = outputs_device_get(session->device_id);
  if (!device)
    return -1;

  snprintf(device_id_hex, sizeof(device_id_hex), "%016" PRIX64, airplay_device_id);

  session->pair_verify_ctx = pair_verify_new(session->pair_type, device->auth_key, NULL, NULL, device_id_hex);
  if (!session->pair_verify_ctx)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Couldn't create verification verify context (invalid auth key?)\n");
      return -1;
    }

  return payload_make_pair_generic(4, req, session);
}

static int
payload_make_pair_verify2(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  return payload_make_pair_generic(5, req, session);
}


/* ------------------------------ Session startup --------------------------- */

static void
start_failure(struct airplay_session *session)
{
  struct output_device *device;

  device = outputs_device_get(session->device_id);
  if (!device)
    {
      session_failure(session);
      return;
    }

  // If our key was incorrect, or the device reset its pairings, then this
  // function was called because the encrypted request (SETUP) timed out.
  //
  // But not every start failure is an auth failure. If the receiver answered
  // us and we simply could not use the answer, the keys are fine and throwing
  // them away is actively harmful: the device drops to requires_auth and the
  // user is sent chasing PIN prompts on a speaker that was never unpaired.
  // Only clear the keys when the failure could plausibly BE an auth failure.
  if (device->auth_key && !session->protocol_error)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Clearing '%s' pairing keys, you need to pair again\n", session->devname);

      free(device->auth_key);
      device->auth_key = NULL;
      device->requires_auth = 1;
    }
  else if (session->protocol_error)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Keeping '%s' pairing keys: the device answered, we could not use the reply\n", session->devname);
    }

  session_failure(session);
}

// Context for a scheduled soft-start/probe retry. The session that failed has
// already been torn down, so the retry is keyed by device id and the pending
// player callback id, and carries the attempt counter forward to keep the
// backoff bounded. seq_type records which sequence to re-run (AIRPLAY_SEQ_START
// or AIRPLAY_SEQ_PROBE).
struct airplay_start_retry_ctx
{
  uint64_t device_id;
  int callback_id;
  int attempt;
  enum airplay_seq_type seq_type;
  struct event *timer;
};

static void
start_retry_timer_cb(int fd, short what, void *arg)
{
  struct airplay_start_retry_ctx *ctx = arg;
  struct output_device *device;
  struct airplay_session *session;

  device = outputs_device_get(ctx->device_id);

  // Device disappeared, or something already started a session in the meantime.
  // Resolve the still-pending activation callback as failed so it never hangs.
  if (!device || device->session)
    {
      outputs_cb(ctx->callback_id, ctx->device_id, OUTPUT_STATE_FAILED);
      goto out;
    }

  session = session_make(device, ctx->callback_id);
  if (!session)
    {
      outputs_cb(ctx->callback_id, ctx->device_id, OUTPUT_STATE_FAILED);
      goto out;
    }

  session->start_retries = ctx->attempt;
  session->probing = (ctx->seq_type == AIRPLAY_SEQ_PROBE);

  DPRINTF(E_INFO, L_AIRPLAY, "Retrying %s for '%s' (attempt %d/%d)\n",
          (ctx->seq_type == AIRPLAY_SEQ_PROBE) ? "probe" : "start",
          device->name, ctx->attempt, AIRPLAY_START_RETRY_MAX);

  if (ctx->seq_type == AIRPLAY_SEQ_PROBE)
    sequence_start(AIRPLAY_SEQ_PROBE, session, NULL, "device_probe (retry)");
  else
    sequence_start(AIRPLAY_SEQ_START, session, NULL, "device_start (retry)");

 out:
  event_free(ctx->timer);
  free(ctx);
}

// Schedule a delayed soft-start/probe retry. On any failure to schedule, the
// pending activation callback is resolved as failed rather than left hanging.
static void
start_retry_schedule(uint64_t device_id, int callback_id, int attempt, enum airplay_seq_type seq_type)
{
  struct airplay_start_retry_ctx *ctx;
  struct timeval tv = { AIRPLAY_START_RETRY_WAIT_SEC, 0 };

  CHECK_NULL(L_AIRPLAY, ctx = calloc(1, sizeof(struct airplay_start_retry_ctx)));

  ctx->device_id = device_id;
  ctx->callback_id = callback_id;
  ctx->attempt = attempt;
  ctx->seq_type = seq_type;
  ctx->timer = evtimer_new(evbase_player, start_retry_timer_cb, ctx);
  if (!ctx->timer)
    {
      free(ctx);
      outputs_cb(callback_id, device_id, OUTPUT_STATE_FAILED);
      return;
    }

  evtimer_add(ctx->timer, &tv);
}

static void
start_retry_impl(struct airplay_session *session, enum airplay_seq_type seq_type)
{
  struct output_device *device;
  int callback_id = session->callback_id;
  int attempt = session->start_retries;

  device = outputs_device_get(session->device_id);
  if (!device)
    {
      session_failure(session);
      return;
    }

  // Some devices don't seem to work with ipv6, so if the error wasn't a hard
  // failure (bad password) we fall back to ipv4 and flag device as bad for ipv6.
  // Only do this one-shot fallback when this was an ipv6 attempt that can
  // actually fall back to a distinct ipv4 endpoint. v6_disabled is sticky
  // (survives session recreation and mDNS re-advertisement), so the fallback is
  // strictly one-shot per device and can never spin.
  if (session->family == AF_INET6 && !(session->state & AIRPLAY_STATE_F_FAILED)
      && !device->v6_disabled && device->v4_address)
    {
      // This flag is permanent and will not be overwritten by mdns advertisements
      device->v6_disabled = 1;

      // Drop session, try again with ipv4
      session_cleanup(session);
      if (seq_type == AIRPLAY_SEQ_PROBE)
        airplay_device_probe(device, callback_id);
      else
        airplay_device_start(device, callback_id);
      return;
    }

  // Hard failure (e.g. bad password): give up immediately, no backoff.
  if (session->state & AIRPLAY_STATE_F_FAILED)
    {
      session_failure(session);
      return;
    }

  // Soft failure (connection refused, GET /info negative, timeout). Retry a
  // bounded number of times with a fixed backoff before reporting failure -
  // gives e.g. a HomePod stereo pair time to finish its post-TEARDOWN reset.
  // session_cleanup() drops the dead session WITHOUT firing the callback, so
  // the player's activation stays pending across the retry.
  if (attempt < AIRPLAY_START_RETRY_MAX)
    {
      start_retry_schedule(session->device_id, callback_id, attempt + 1, seq_type);
      session_cleanup(session);
      return;
    }

  session_failure(session);
}

static void
start_retry(struct airplay_session *session)
{
  start_retry_impl(session, AIRPLAY_SEQ_START);
}

static void
probe_retry(struct airplay_session *session)
{
  start_retry_impl(session, AIRPLAY_SEQ_PROBE);
}


/* ---------------------------- RTSP response handlers ---------------------- */

static enum airplay_seq_type
response_handler_pin_start(struct evrtsp_request *req, struct airplay_session *session)
{
  session->state = AIRPLAY_STATE_AUTH;

  return AIRPLAY_SEQ_CONTINUE; // TODO before we reported failure since device is locked
}

static enum airplay_seq_type
response_handler_record(struct evrtsp_request *req, struct airplay_session *session)
{
  session->state = AIRPLAY_STATE_RECORD;

  return AIRPLAY_SEQ_CONTINUE;
}

static enum airplay_seq_type
response_handler_setup_stream(struct evrtsp_request *req, struct airplay_session *session)
{
  plist_t response;
  plist_t streams;
  plist_t stream;
  plist_t stream_connections;
  plist_t stream_connection_rtp;
  plist_t item;
  uint64_t uintval;
  int ret;

  DPRINTF(E_INFO, L_AIRPLAY, "Setting up AirPlay session %u to %s\n", session->session_id, session->address);

  ret = wplist_from_evbuf(&response, req->input_buffer);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not parse plist from '%s'\n", session->devname);
      return AIRPLAY_SEQ_ABORT;
    }

  streams = plist_dict_get_item(response, "streams");
  if (!streams)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not find streams item in response from '%s'\n", session->devname);
      goto error;
    }

  stream = plist_array_get_item(streams, 0);
  if (!stream)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not find stream item in response from '%s'\n", session->devname);
      goto error;
    }

  item = plist_dict_get_item(stream, "controlPort");
  if (item)
    {
      plist_get_uint_val(item, &uintval);
      session->control_port = uintval;
    }

  if (session->buffered_mode)
    {
      // A receiver that honours the streamConnections dict we sent in
      // SETUP(stream) answers in kind: the data port comes back as
      // streamConnections.streamConnectionTypeRTP.streamConnectionKeyPort
      // rather than as a flat dataPort. Prefer that, and fall back to
      // dataPort for receivers that ignore streamConnections.
      stream_connections = plist_dict_get_item(stream, "streamConnections");
      if (stream_connections)
	{
	  stream_connection_rtp = plist_dict_get_item(stream_connections, "streamConnectionTypeRTP");
	  if (stream_connection_rtp)
	    {
	      item = plist_dict_get_item(stream_connection_rtp, "streamConnectionKeyPort");
	      if (item)
		{
		  plist_get_uint_val(item, &uintval);
		  session->buffered_data_port = uintval;
		  DPRINTF(E_DBG, L_AIRPLAY, "Buffered data port from streamConnections for '%s': %u\n",
		          session->devname, session->buffered_data_port);
		}
	    }
	}

      if (session->buffered_data_port == 0)
	{
	  item = plist_dict_get_item(stream, "dataPort");
	  if (item)
	    {
	      plist_get_uint_val(item, &uintval);
	      session->buffered_data_port = uintval;
	    }
	}

      if (session->buffered_data_port == 0)
	{
	  // A missing data port means the receiver answered but we cannot use
	  // the reply; this is not an auth failure, so pairing keys are kept
	  // (see session->protocol_error and start_failure()).
	  session->protocol_error = true;
	  DPRINTF(E_LOG, L_AIRPLAY, "Missing data port in buffered SETUP reply from '%s'\n", session->devname);
	  goto error;
	}

      DPRINTF(E_DBG, L_AIRPLAY, "Negotiated buffered streaming session; ports d=%u c=%u e=%u\n", session->buffered_data_port, session->control_port, session->events_port);

      ret = airplay_buffered_start(&session->buffered, evbase_player, session->address, session->buffered_data_port,
                                    session->shared_secret, session->shared_secret_len, session->buffered_format_id << 24);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_AIRPLAY, "Could not start buffered AirPlay stream to '%s'\n", session->devname);
	  goto error;
	}
    }
  else
    {
      item = plist_dict_get_item(stream, "dataPort");
      if (item)
	{
	  plist_get_uint_val(item, &uintval);
	  session->data_port = uintval;
	}

      if (session->data_port == 0 || session->control_port == 0)
	{
	  DPRINTF(E_LOG, L_AIRPLAY, "Missing port number in reply from '%s' (d=%u, c=%u)\n", session->devname, session->data_port, session->control_port);
	  goto error;
	}

      DPRINTF(E_DBG, L_AIRPLAY, "Negotiated UDP streaming session; ports d=%u c=%u e=%u\n", session->data_port, session->control_port, session->events_port);

      session->server_fd = net_connect(session->address, session->data_port, SOCK_DGRAM, "AirPlay data");
      if (session->server_fd < 0)
	{
	  DPRINTF(E_LOG, L_AIRPLAY, "Could not connect to data port of '%s'\n", session->devname);
	  goto error;
	}
    }

  session->state = AIRPLAY_STATE_SETUP;

  plist_free(response);
  return AIRPLAY_SEQ_CONTINUE;

 error:
  plist_free(response);
  return AIRPLAY_SEQ_ABORT;
}

static enum airplay_seq_type
response_handler_volume_start(struct evrtsp_request *req, struct airplay_session *session)
{
  int ret;

  ret = airplay_metadata_startup_send(session);
  if (ret < 0)
    return AIRPLAY_SEQ_ABORT;

  return AIRPLAY_SEQ_CONTINUE;
}

static int
handle_timingpeerinfo(uint32_t *slave_id, plist_t response, const char *local_v4_address, const char *local_v6_address)
{
  int family = local_v6_address ? AF_INET6 : AF_INET;
  plist_t item;
  plist_t peer_addresses;
  plist_t peer_address;
  const char *ptr;
  union net_sockaddr naddr;
  char peer_straddress[128];
  char ifname[32];
  char *response_str;
  int ret;
  int i;

  item = plist_dict_get_item(response, "timingPeerInfo");
  if (!item)
    goto error;

  peer_addresses = plist_dict_get_item(item, "Addresses");
  if (!peer_addresses)
    goto error;

  // Walk through addresses to get one from the right family
  for (i = 0; (peer_address = plist_array_get_item(peer_addresses, i)); i++)
    {
      ptr = plist_get_string_ptr(peer_address, NULL);
      if (!ptr)
	continue;

      DPRINTF(E_SPAM, L_AIRPLAY, "Checking timing peer '%s'\n", ptr);

      // If address is ipv6 and ipv6 is not enabled, this will return -1
      ret = net_sockaddr_get(&naddr, ptr, 0);
      if (ret < 0 || naddr.sa.sa_family != family)
	continue;

      // Append %ifname to ipv6 because libairptpd needs it to send if the address is link-local (fe80)
      if (family == AF_INET6 && net_if_get(ifname, sizeof(ifname), local_v6_address) == 0)
	snprintf(peer_straddress, sizeof(peer_straddress), "%s%%%s", ptr, ifname);
      else if (family == AF_INET)
	snprintf(peer_straddress, sizeof(peer_straddress), "%s", ptr);
      else
	continue;

      // Just add the first good address
      ret = ptpd_slave_add(slave_id, peer_straddress);
      if (ret == 0)
	return 0;
    }

 error:
  response_str = wplist_to_xml(response);
  DPRINTF(E_LOG, L_AIRPLAY, "Error reading speaker's timing peer info (%s, %s):\n%s\n", local_v4_address, local_v6_address, response_str);
  free(response_str);
  return -1;
}

static enum airplay_seq_type
response_handler_setup_session(struct evrtsp_request *req, struct airplay_session *session)
{
  plist_t response;
  plist_t item;
  uint64_t uintval;
  int ret;
  if (req->response_code == RTSP_UNAUTHORIZED)
    {
      if (session->req_has_auth)
	{
	  DPRINTF(E_LOG, L_AIRPLAY, "Bad or missing password for device '%s' (%s)\n", session->devname, session->address);
	  return AIRPLAY_SEQ_ABORT;
	}

      // We haven't tried authenticating yet, so save realm and nonce from the
      // received WWW-Authenticate header and trigger a re-run with auth header
      ret = auth_header_parse(session, req);
      if (ret < 0)
	return AIRPLAY_SEQ_ABORT;

      return AIRPLAY_SEQ_START_PLAYBACK;
    }
  else if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_WARN, L_AIRPLAY, "Unexpected reply to SETUP (session) from '%s'\n", session->devname);
      return AIRPLAY_SEQ_ABORT;
    }

  ret = wplist_from_evbuf(&response, req->input_buffer);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not parse plist from '%s'\n", session->devname);
      return AIRPLAY_SEQ_ABORT;
    }

  item = plist_dict_get_item(response, "eventPort");
  if (item)
    {
      plist_get_uint_val(item, &uintval);
      session->events_port = uintval;
    }

  if (session->events_port == 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "SETUP reply is missing event port\n");
      goto error;
    }

  // Reverse connection, used to receive playback events from device
  ret = airplay_events_listen(session->devname, session->address, session->events_port, session->shared_secret, session->shared_secret_len);
  if (ret < 0)
    {
      DPRINTF(E_WARN, L_AIRPLAY, "Could not connect to '%s' events port %u, proceeding anyway\n", session->devname, session->events_port);
    }

  if (session->master_session->use_ptp)
    {
      ret = handle_timingpeerinfo(&session->ptpd_slave_id, response, session->local_v4_address, session->local_v6_address);
      if (ret < 0)
	goto error;
    }

  plist_free(response);
  return AIRPLAY_SEQ_CONTINUE;

 error:
  plist_free(response);
  return AIRPLAY_SEQ_ABORT;
}

static enum airplay_seq_type
response_handler_flush(struct evrtsp_request *req, struct airplay_session *session)
{
  session->state = AIRPLAY_STATE_CONNECTED;
  return AIRPLAY_SEQ_CONTINUE;
}

// A FLUSHBUFFERED invalidates the receiver's anchor, so a fresh
// SETRATEANCHORTIME must be sent before playback resumes (anchor_pending);
// airplay_write() picks this up on the next CONNECTED->STREAMING transition.
static enum airplay_seq_type
response_handler_flush_buffered(struct evrtsp_request *req, struct airplay_session *session)
{
  session->state = AIRPLAY_STATE_CONNECTED;
  session->anchor_pending = true;
  session->anchor_confirmed = false; // Resumed audio must wait for the fresh anchor
  session->master_session->buffered_marker_pending = true;
  // Playback resumes at a new wall time, so the old rtptime->networkTime
  // mapping no longer holds; the next anchor establishes a fresh one
  session->master_session->buffered_anchor_mapped = false;
  // Queued-but-unsent frames would otherwise carry seqnums past flushUntilSeq,
  // and the receiver keeps those instead of discarding them.
  if (session->master_session && session->master_session->encoder)
    airplay_encoder_flush(session->master_session->encoder);
  return AIRPLAY_SEQ_CONTINUE;
}

static bool
master_session_has_confirmed_anchor(struct airplay_master_session *ams)
{
  struct airplay_session *session;

  for (session = airplay_sessions; session; session = session->next)
    {
      if (session->master_session == ams && session->buffered_mode && session->anchor_confirmed)
	return true;
    }

  return false;
}

// Runs with proceed_on_rtsp_not_ok, since a rejected anchor is retryable: the
// suspected cause of an in-band or RTSP-level rejection is that the anchor
// raced the receiver's lock onto our PTP timeline (it can arrive under a
// second after SETPEERS). Audio written in the meantime is just held by the
// receiver, so streaming state is unaffected while we retry.
static enum airplay_seq_type
response_handler_send_anchor(struct evrtsp_request *req, struct airplay_session *session)
{
  struct timeval tv = { AIRPLAY_BUFFERED_ANCHOR_RETRY_WAIT_MS / 1000, (AIRPLAY_BUFFERED_ANCHOR_RETRY_WAIT_MS % 1000) * 1000 };
  plist_t response = NULL;
  char *xml;
  size_t len;
  int64_t inband_status = 0;

  // Some receivers can return 200 OK while rejecting the anchor IN-BAND with
  // a plist body carrying a nonzero status. Parse the body once up front;
  // acceptance requires 200 AND in-band status 0.
  len = evbuffer_get_length(req->input_buffer);
  if (len > 0 && wplist_from_evbuf(&response, req->input_buffer) == 0)
    {
      plist_t item = plist_dict_get_item(response, "status");
      if (item)
	plist_get_int_val(item, &inband_status);
    }

  if (req->response_code == RTSP_OK && inband_status == 0)
    {
      if (response)
	plist_free(response);
      DPRINTF(E_INFO, L_AIRPLAY, "SETRATEANCHORTIME accepted by '%s', starting buffered audio\n", session->devname);
      session->anchor_confirmed = true;
      session->anchor_retries_left = AIRPLAY_BUFFERED_ANCHOR_RETRIES_MAX;
      return AIRPLAY_SEQ_CONTINUE;
    }

  // The response body may say why the anchor was rejected
  if (response)
    {
      xml = wplist_to_xml(response);
      if (xml)
	{
	  DPRINTF(E_LOG, L_AIRPLAY, "SETRATEANCHORTIME error response from '%s' (rtsp %d, in-band status %" PRIi64 "):\n%s\n",
	          session->devname, req->response_code, inband_status, xml);
	  free(xml);
	}
      plist_free(response);
    }
  else if (len > 0)
    DHEXDUMP(E_LOG, L_AIRPLAY, evbuffer_pullup(req->input_buffer, -1), (int)len, "SETRATEANCHORTIME error response (not a plist)\n");

  if (session->anchor_retries_left <= 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "SETRATEANCHORTIME repeatedly rejected by '%s', giving up\n", session->devname);
      return AIRPLAY_SEQ_ABORT;
    }

  session->anchor_retries_left--;
  DPRINTF(E_LOG, L_AIRPLAY, "SETRATEANCHORTIME rejected by '%s' (%d %s), retrying in %d ms (%d retries left)\n",
          session->devname, req->response_code, req->response_code_line, AIRPLAY_BUFFERED_ANCHOR_RETRY_WAIT_MS, session->anchor_retries_left);

  // If no receiver is playing off the stored mapping yet, drop it so the
  // retry computes a fresh now+lead anchor (a stale point drifts into the
  // past across retries). Once any session has a confirmed anchor the mapping
  // is live and the retry must re-send the same point.
  if (!master_session_has_confirmed_anchor(session->master_session))
    session->master_session->buffered_anchor_mapped = false;

  evtimer_add(session->anchorev, &tv);

  return AIRPLAY_SEQ_CONTINUE;
}

static enum airplay_seq_type
response_handler_teardown(struct evrtsp_request *req, struct airplay_session *session)
{
  session->state = AIRPLAY_STATE_STOPPED;
  return AIRPLAY_SEQ_CONTINUE;
}

static enum airplay_seq_type
response_handler_teardown_failure(struct evrtsp_request *req, struct airplay_session *session)
{
  if (session->state != AIRPLAY_STATE_AUTH)
    session->state = AIRPLAY_STATE_FAILED;
  return AIRPLAY_SEQ_CONTINUE;
}

// Parses the receiver's advertised buffered-transport (type 103) capability
// from a GET /info response into an output_mode bitmask (the
// OUTPUT_MODE_AIRPLAY2_BUFFERED*/SURROUND* bits). Receivers declare it two
// ways: supportedAudioFormatsExtended/bufferStream as an array of format ids,
// or supportedFormats/bufferStream as an integer bitmask with the format ids
// as bit positions (the same convention SETUP(stream)'s audioFormat field
// uses, see AIRPLAY_BUFFERED_AUDIOFORMAT). Format 39 (5.1) sets BOTH surround
// bits: the wire capability only says the receiver accepts a 5.1 buffered
// stream, not which of the two ways owntone might fill the channels, so both
// modes are recorded as available and the standalone-ATV gate (applied at
// selection/report time, not here) decides whether either is actually offered.
static uint32_t
buffered_modes_parse(plist_t response)
{
  uint32_t modes = 0;
  plist_t item;
  plist_t buffer_stream;

  item = plist_dict_get_item(response, "supportedAudioFormatsExtended");
  if (item)
    {
      buffer_stream = plist_dict_get_item(item, "bufferStream");
      if (buffer_stream)
	{
	  uint32_t n = plist_array_get_size(buffer_stream);
	  plist_t elm;
	  uint64_t fmt;
	  uint32_t idx;

	  for (idx = 0; idx < n; idx++)
	    {
	      elm = plist_array_get_item(buffer_stream, idx);
	      if (!elm)
		continue;

	      plist_get_uint_val(elm, &fmt);
	      if (fmt == AIRPLAY_FORMAT_ID_AAC_STEREO)
		modes |= OUTPUT_MODE_AIRPLAY2_BUFFERED;
	      else if (fmt == AIRPLAY_FORMAT_ID_ALAC48K_24_STEREO)
		modes |= OUTPUT_MODE_AIRPLAY2_BUFFERED_24;
	      else if (fmt == AIRPLAY_FORMAT_ID_AAC_51)
		modes |= OUTPUT_MODE_AIRPLAY2_SURROUND_STEREO | OUTPUT_MODE_AIRPLAY2_SURROUND_UPMIX;
	    }
	}
    }

  item = plist_dict_get_item(response, "supportedFormats");
  if (item)
    {
      buffer_stream = plist_dict_get_item(item, "bufferStream");
      if (buffer_stream)
	{
	  uint64_t mask = 0;

	  plist_get_uint_val(buffer_stream, &mask);
	  if (mask & AIRPLAY_BUFFERED_AUDIOFORMAT(AIRPLAY_FORMAT_ID_AAC_STEREO))
	    modes |= OUTPUT_MODE_AIRPLAY2_BUFFERED;
	  if (mask & AIRPLAY_BUFFERED_AUDIOFORMAT(AIRPLAY_FORMAT_ID_ALAC48K_24_STEREO))
	    modes |= OUTPUT_MODE_AIRPLAY2_BUFFERED_24;
	  if (mask & AIRPLAY_BUFFERED_AUDIOFORMAT(AIRPLAY_FORMAT_ID_AAC_51))
	    modes |= OUTPUT_MODE_AIRPLAY2_SURROUND_STEREO | OUTPUT_MODE_AIRPLAY2_SURROUND_UPMIX;
	}
    }

  return modes;
}

static enum airplay_seq_type
response_handler_info_generic(struct evrtsp_request *req, struct airplay_session *session)
{
  struct output_device *device;
  plist_t response;
  plist_t item;
  char *response_str;
  int ret;

  device = outputs_device_get(session->device_id);
  if (!device)
    return AIRPLAY_SEQ_ABORT;

  ret = session_ids_set(session);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not make session url or id for device '%s'\n", session->devname);
      return AIRPLAY_SEQ_ABORT;
    }

  ret = wplist_from_evbuf(&response, req->input_buffer);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not parse plist from '%s'\n", session->devname);
      return AIRPLAY_SEQ_ABORT;
    }

  item = plist_dict_get_item(response, "statusFlags");
  if (item)
    plist_get_uint_val(item, &session->statusflags);

  // Buffered AirPlay 2 capability gate. Parse the full bufferStream capability
  // list into an output_mode bitmask (not just whichever format id this
  // session happens to want), so the result can both drive this session's
  // decision and be cached/persisted on the device for the mode-selection API.
  {
    uint32_t advertised_buffered_modes = buffered_modes_parse(response);
    uint32_t wanted_mode_bit;

    // 5.1 surround output is offered only to a standalone Apple TV. Other
    // AirPlay 2 receivers (e.g. HomePods) may advertise the 5.1 bufferStream
    // format, but we do not present surround output for them, so strip the two
    // surround bits here before the capability is cached and reported.
    if (!airplay_device_allows_surround(device))
      advertised_buffered_modes &= ~(OUTPUT_MODE_AIRPLAY2_SURROUND_STEREO | OUTPUT_MODE_AIRPLAY2_SURROUND_UPMIX);

    switch (session->wants_buffered_kind)
      {
        case AIRPLAY_BUFFERED_KIND_ALAC24:          wanted_mode_bit = OUTPUT_MODE_AIRPLAY2_BUFFERED_24;     break;
        case AIRPLAY_BUFFERED_KIND_SURROUND_STEREO: wanted_mode_bit = OUTPUT_MODE_AIRPLAY2_SURROUND_STEREO; break;
        case AIRPLAY_BUFFERED_KIND_SURROUND_UPMIX:  wanted_mode_bit = OUTPUT_MODE_AIRPLAY2_SURROUND_UPMIX;  break;
        case AIRPLAY_BUFFERED_KIND_AAC_STEREO:
        case AIRPLAY_BUFFERED_KIND_NONE:
        default:                                    wanted_mode_bit = OUTPUT_MODE_AIRPLAY2_BUFFERED;        break;
      }
    session->device_supports_buffered = (advertised_buffered_modes & wanted_mode_bit) != 0;

    if (advertised_buffered_modes != device->buffered_modes)
      {
        outputs_device_buffered_modes_set(device, advertised_buffered_modes);
        db_speaker_save(device);
      }
  }

  // The full /info plist is the only place a receiver declares its audio
  // capabilities (audioFormats, output device attributes), which we will need
  // for multichannel output detection. Dump it so a debug log captures it.
  response_str = wplist_to_xml(response);
  DPRINTF(E_DBG, L_AIRPLAY, "GET /info response from '%s':\n%s\n", session->devname, response_str);
  free(response_str);

  plist_free(response);

  DPRINTF(E_DBG, L_AIRPLAY, "Status flags from '%s' was %" PRIu64 ": cable attached %d, one time pairing %d, password %d, PIN %d\n",
    session->devname, session->statusflags, (bool)(session->statusflags & AIRPLAY_FLAG_AUDIO_CABLE_ATTACHED), (bool)(session->statusflags & AIRPLAY_FLAG_ONE_TIME_PAIRING_REQUIRED),
    (bool)(session->statusflags & AIRPLAY_FLAG_PASSWORD_REQUIRED), (bool)(session->statusflags & AIRPLAY_FLAG_PIN_REQUIRED));

  // Buffered mode decision: only relevant if the device is configured for it
  // (session->wants_buffered_kind != NONE). If the device also advertises the
  // capability (checked against whichever format id session->buffered_format_id
  // resolved to - the capability check above is generic across all buffered
  // kinds), swap in a buffered master session; a failure to do so aborts the
  // session outright (no silent downgrade mid negotiation). If the device
  // doesn't advertise it, we simply proceed with the realtime ALAC master
  // session created in session_make().
  // A capability probe must never touch admission, encoders, or ams state:
  // it funnels into this same handler but the session tears down right
  // after, and running the swap here would spawn a throwaway encoder thread
  // and could fail the probe merely because other outputs hold the budget.
  if (!session->probing && session->wants_buffered_kind != AIRPLAY_BUFFERED_KIND_NONE)
    {
      if (session->device_supports_buffered)
	{
	  struct airplay_master_session *new_ams;
	  struct airplay_master_session *old_ams;

	  new_ams = master_session_make(&device->quality, session->master_session->use_ptp, session->wants_buffered_kind);
	  if (!new_ams)
	    {
	      DPRINTF(E_LOG, L_AIRPLAY, "Could not create buffered master session for '%s'\n", session->devname);

	      // A budget rejection is distinguishable from any other setup
	      // failure so it can surface as a capacity-specific API error;
	      // either way this is a hard failure, not a soft retry candidate.
	      if (encoder_budget_rejected)
	        device->last_failure = OUTPUT_FAILURE_CAPACITY;
	      session->state = AIRPLAY_STATE_FAILED;
	      return AIRPLAY_SEQ_ABORT;
	    }

	  // Swap the pointer before cleaning up the old master session: cleanup
	  // only frees a master session that no session references any more, so
	  // it must run after this session has been repointed at new_ams, or it
	  // will see this session still attached to old_ams and refuse to free
	  // it (leaking it, since nothing else may ever reference it again).
	  old_ams = session->master_session;
	  session->master_session = new_ams;
	  master_session_cleanup(old_ams);

	  session->buffered_mode = true;
	  session->anchor_pending = true;

	  DPRINTF(E_INFO, L_AIRPLAY, "Device '%s' advertises buffered format id %u; streaming via buffered protocol (kind=%d)\n",
	          session->devname, session->buffered_format_id, session->wants_buffered_kind);
	}
      else
	{
	  DPRINTF(E_INFO, L_AIRPLAY, "Device '%s' is configured for buffered mode, but does not advertise buffered format id %u; falling back to realtime ALAC\n",
	          session->devname, session->buffered_format_id);
	}
    }

  // Identify next sequence based on response
  if (session->statusflags & AIRPLAY_FLAG_ONE_TIME_PAIRING_REQUIRED)
    {
      session->pair_type = PAIR_CLIENT_HOMEKIT_NORMAL;

      if (!device->auth_key)
	{
	  device->requires_auth = 1;
          session->state = AIRPLAY_STATE_AUTH;
	  return AIRPLAY_SEQ_PIN_START;
	}

      session->state = AIRPLAY_STATE_INFO;
      return AIRPLAY_SEQ_PAIR_VERIFY;
    }
  else if (session->statusflags & AIRPLAY_FLAG_PIN_REQUIRED)
    {
      free(device->auth_key);
      device->auth_key = NULL;
      device->requires_auth = 1;

      session->pair_type = PAIR_CLIENT_HOMEKIT_NORMAL;
      session->state = AIRPLAY_STATE_AUTH;
      return AIRPLAY_SEQ_PIN_START;
    }
  else if (session->statusflags & AIRPLAY_FLAG_PASSWORD_REQUIRED)
    {
      session->pair_type = PAIR_CLIENT_HOMEKIT_NORMAL;

      if (!session->password)
	{
	  DPRINTF(E_LOG, L_AIRPLAY, "'%s' requires password authentication, but none given in config\n", session->devname);
	  return AIRPLAY_SEQ_ABORT;
	}
      else if (!device->auth_key)
	{
          session->state = AIRPLAY_STATE_AUTH;
	  return AIRPLAY_SEQ_PAIR_SETUP;
	}

      session->state = AIRPLAY_STATE_INFO;
      return AIRPLAY_SEQ_PAIR_VERIFY;
    }

  session->pair_type = PAIR_CLIENT_HOMEKIT_TRANSIENT;
  session->state = AIRPLAY_STATE_INFO;
  return AIRPLAY_SEQ_PAIR_TRANSIENT;
}

static enum airplay_seq_type
response_handler_info_probe(struct evrtsp_request *req, struct airplay_session *session)
{
  enum airplay_seq_type seq_type;

  seq_type = response_handler_info_generic(req, session);
  if (seq_type == AIRPLAY_SEQ_PAIR_TRANSIENT || seq_type == AIRPLAY_SEQ_PAIR_VERIFY)
    seq_type = AIRPLAY_SEQ_CONTINUE; // When probing we don't proceed to PAIR_TRANSIENT/VERIFY

  return seq_type;
}

static enum airplay_seq_type
response_handler_info_start(struct evrtsp_request *req, struct airplay_session *session)
{
  enum airplay_seq_type seq_type;

  seq_type = response_handler_info_generic(req, session);
  if (seq_type != AIRPLAY_SEQ_ABORT && seq_type != AIRPLAY_SEQ_PIN_START)
    session->next_seq = AIRPLAY_SEQ_START_PLAYBACK; // Pair and then run SEQ_START_PLAYBACK which sets up the playback

  return seq_type;
}

static enum airplay_seq_type
response_handler_pair_generic(int step, struct evrtsp_request *req, struct airplay_session *session)
{
  uint8_t *response;
  const char *errmsg;
  size_t len;
  int ret;

  response = evbuffer_pullup(req->input_buffer, -1);
  len = evbuffer_get_length(req->input_buffer);

  switch (step)
    {
      case 1:
	ret = pair_setup_response1(session->pair_setup_ctx, response, len);
	errmsg = pair_setup_errmsg(session->pair_setup_ctx);
	break;
      case 2:
	ret = pair_setup_response2(session->pair_setup_ctx, response, len);
	errmsg = pair_setup_errmsg(session->pair_setup_ctx);
	break;
      case 3:
	ret = pair_setup_response3(session->pair_setup_ctx, response, len);
	errmsg = pair_setup_errmsg(session->pair_setup_ctx);
	break;
      case 4:
	ret = pair_verify_response1(session->pair_verify_ctx, response, len);
	errmsg = pair_verify_errmsg(session->pair_verify_ctx);
	break;
      case 5:
	ret = pair_verify_response2(session->pair_verify_ctx, response, len);
	errmsg = pair_verify_errmsg(session->pair_verify_ctx);
	break;
      default:
	ret = -1;
	errmsg = "Bug! Bad step number";
    }

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Pairing step %d response from '%s' error: %s\n", step, session->devname, errmsg);
      DHEXDUMP(E_DBG, L_AIRPLAY, response, len, "Raw response");
      return AIRPLAY_SEQ_ABORT;
    }

  return AIRPLAY_SEQ_CONTINUE;
}

static enum airplay_seq_type
response_handler_pair_setup1(struct evrtsp_request *req, struct airplay_session *session)
{
  struct output_device *device;

  if (session->pair_type == PAIR_CLIENT_HOMEKIT_TRANSIENT && req->response_code == RTSP_CONNECTION_AUTH_REQUIRED)
    {
      device = outputs_device_get(session->device_id);
      if (!device)
	return AIRPLAY_SEQ_ABORT;

      device->requires_auth = 1; // Sticky on the canonical, see effective_candidate_apply()
      session->pair_type = PAIR_CLIENT_HOMEKIT_NORMAL;

      return AIRPLAY_SEQ_PIN_START;
    }

  return response_handler_pair_generic(1, req, session);
}

static enum airplay_seq_type
response_handler_pair_setup2(struct evrtsp_request *req, struct airplay_session *session)
{
  enum airplay_seq_type seq_type;
  struct pair_result *result;
  int ret;

  seq_type = response_handler_pair_generic(2, req, session);
  if (seq_type != AIRPLAY_SEQ_CONTINUE)
    return seq_type;

  if (session->pair_type != PAIR_CLIENT_HOMEKIT_TRANSIENT)
    return seq_type;

  ret = pair_setup_result(NULL, &result, session->pair_setup_ctx);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Transient setup result error: %s\n", pair_setup_errmsg(session->pair_setup_ctx));
      goto error;
    }

  ret = session_cipher_setup(session, result->shared_secret, result->shared_secret_len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Pair transient error setting up encryption for '%s'\n", session->devname);
      goto error;
    }

  return AIRPLAY_SEQ_CONTINUE;

 error:
  session->state = AIRPLAY_STATE_FAILED;
  return AIRPLAY_SEQ_ABORT;
}

static enum airplay_seq_type
response_handler_pair_setup3(struct evrtsp_request *req, struct airplay_session *session)
{
  struct output_device *device;
  const char *authorization_key;
  enum airplay_seq_type seq_type;
  int ret;

  seq_type = response_handler_pair_generic(3, req, session);
  if (seq_type != AIRPLAY_SEQ_CONTINUE)
    return seq_type;

  ret = pair_setup_result(&authorization_key, NULL, session->pair_setup_ctx);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Pair setup result error: %s\n", pair_setup_errmsg(session->pair_setup_ctx));
      return AIRPLAY_SEQ_ABORT;
    }

  DPRINTF(E_LOG, L_AIRPLAY, "Pair setup stage complete, saving authorization key\n");

  device = outputs_device_get(session->device_id);
  if (!device)
    return AIRPLAY_SEQ_ABORT;

  free(device->auth_key);
  device->auth_key = strdup(authorization_key);

  // A blocking db call... :-~
  db_speaker_save(device);

  // No longer AIRPLAY_STATE_AUTH
  session->state = AIRPLAY_STATE_STOPPED;

  return AIRPLAY_SEQ_CONTINUE;
}

static enum airplay_seq_type
response_handler_pair_verify1(struct evrtsp_request *req, struct airplay_session *session)
{
  struct output_device *device;
  enum airplay_seq_type seq_type;

  seq_type = response_handler_pair_generic(4, req, session);
  if (seq_type != AIRPLAY_SEQ_CONTINUE)
    {
      session->state = AIRPLAY_STATE_AUTH;

      device = outputs_device_get(session->device_id);
      if (!device)
	return AIRPLAY_SEQ_ABORT;

      // Clear auth_key, the device did not accept it
      free(device->auth_key);
      device->auth_key = NULL;

      return AIRPLAY_SEQ_ABORT;
    }

  return seq_type;
}

static enum airplay_seq_type
response_handler_pair_verify2(struct evrtsp_request *req, struct airplay_session *session)
{
  struct output_device *device;
  enum airplay_seq_type seq_type;
  struct pair_result *result;
  int ret;

  seq_type = response_handler_pair_generic(5, req, session);
  if (seq_type != AIRPLAY_SEQ_CONTINUE)
    goto error;

  ret = pair_verify_result(&result, session->pair_verify_ctx);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Pair verify result error: %s\n", pair_verify_errmsg(session->pair_verify_ctx));
      goto error;
    }

  ret = session_cipher_setup(session, result->shared_secret, result->shared_secret_len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Pair verify error setting up encryption for '%s'\n", session->devname);
      goto error;
    }

  return AIRPLAY_SEQ_CONTINUE;

 error:
  device = outputs_device_get(session->device_id);
  if (!device)
    return AIRPLAY_SEQ_ABORT;

  // Clear auth_key, the device did not accept it, or some other unexpected error
  free(device->auth_key);
  device->auth_key = NULL;

  session->state = AIRPLAY_STATE_AUTH;

  return AIRPLAY_SEQ_ABORT;
}


/* ---------------------- Request/response sequence control ----------------- */

/*
 * Request queueing HOWTO
 *
 * Sending:
 * - increment session->reqs_in_flight
 * - set evrtsp connection closecb to NULL
 *
 * Request callback:
 * - decrement session->reqs_in_flight first thing, even if the callback is
 *   called for error handling (req == NULL or HTTP error code)
 * - if session->reqs_in_flight == 0, setup evrtsp connection closecb
 *
 * When a request fails, the whole AirPlay session is declared failed and
 * torn down by calling session_failure(), even if there are requests
 * queued on the evrtsp connection. There is no reason to think pending
 * requests would work out better than the one that just failed and recovery
 * would be tricky to get right.
 *
 * evrtsp behaviour with queued requests:
 * - request callback is called with req == NULL to indicate a connection
 *   error; if there are several requests queued on the connection, this can
 *   happen for each request if the connection isn't destroyed
 * - the connection is reset, and the closecb is called if the connection was
 *   previously connected. There is no closecb set when there are requests in
 *   flight
 */

static struct airplay_seq_definition airplay_seq_definition[] =
{
  { AIRPLAY_SEQ_START, NULL, start_retry },
  { AIRPLAY_SEQ_START_PLAYBACK, session_connected, start_failure },
  { AIRPLAY_SEQ_PROBE, session_success, probe_retry },
  { AIRPLAY_SEQ_FLUSH, session_status, session_failure },
  { AIRPLAY_SEQ_STOP, session_success, session_failure },
  { AIRPLAY_SEQ_FAILURE, session_success, session_failure},
  { AIRPLAY_SEQ_PIN_START, session_success, session_failure },
  { AIRPLAY_SEQ_SEND_VOLUME, session_status, session_failure },
  { AIRPLAY_SEQ_SEND_TEXT, NULL, session_failure },
  { AIRPLAY_SEQ_SEND_PROGRESS, NULL, session_failure },
  { AIRPLAY_SEQ_SEND_ARTWORK, NULL, session_failure },
  { AIRPLAY_SEQ_PAIR_SETUP, session_pair_success, session_failure },
  { AIRPLAY_SEQ_PAIR_VERIFY, session_pair_success, session_failure },
  // Transient pairing only runs during session start (probe converts it to
  // CONTINUE), so a failure here is a start failure. Use start_retry: HomePod
  // stereo pairs sporadically reject transient pair-setup with an SRP auth
  // error shortly after a TEARDOWN, and a delayed retry reliably succeeds.
  { AIRPLAY_SEQ_PAIR_TRANSIENT, session_pair_success, start_retry },
  { AIRPLAY_SEQ_FEEDBACK, NULL, session_failure },
  { AIRPLAY_SEQ_SEND_ANCHOR, NULL, session_failure },
  { AIRPLAY_SEQ_FLUSH_BUFFERED, session_status, session_failure },
};

// The size of the second array dimension MUST at least be the size of largest
// sequence + 1, because then we can count on a zero terminator when iterating.
// The buffered start adds three POSTs to START_PLAYBACK (which is also the
// longest sequence), so size for that plus the optional auth-setup step and
// the terminator.
static struct airplay_seq_request airplay_seq_request[][10] =
{
  {
    { AIRPLAY_SEQ_START, "GET /info", EVRTSP_REQ_GET, NULL, response_handler_info_start, NULL, "/info", false },
  },
  {
#if AIRPLAY_USE_AUTH_SETUP
    { AIRPLAY_SEQ_START_PLAYBACK, "auth-setup", EVRTSP_REQ_POST, payload_make_auth_setup, NULL, "application/octet-stream", "/auth-setup", true },
#endif
    // proceed_on_rtsp_not_ok is true because a device may reply with 401 Unauthorized
    // and a WWW-Authenticate header, and then we may need re-run with password auth
    { AIRPLAY_SEQ_START_PLAYBACK, "SETUP (session)", EVRTSP_REQ_SETUP, payload_make_setup_session, response_handler_setup_session, "application/x-apple-binary-plist", NULL, true },
    { AIRPLAY_SEQ_START_PLAYBACK, "RECORD", EVRTSP_REQ_RECORD, payload_make_record, response_handler_record, NULL, NULL, false },
    { AIRPLAY_SEQ_START_PLAYBACK, "SETPEERS", EVRTSP_REQ_SETPEERS, payload_make_setpeers, NULL, "/peer-list-changed", NULL, false },
    // Buffered mode only (payload_make skips them otherwise): the MediaRemote
    // command table and audio-mode POST bracket SETUP(stream), see
    // payload_make_command_supported_commands
    { AIRPLAY_SEQ_START_PLAYBACK, "POST /command", EVRTSP_REQ_POST, payload_make_command_supported_commands, NULL, "application/x-apple-binary-plist", "/command", false },
    // The empty command table is followed by the full one
    { AIRPLAY_SEQ_START_PLAYBACK, "POST /command (full)", EVRTSP_REQ_POST, payload_make_command_supported_commands_full, NULL, "application/x-apple-binary-plist", "/command", false },
    { AIRPLAY_SEQ_START_PLAYBACK, "SETUP (stream)", EVRTSP_REQ_SETUP, payload_make_setup_stream, response_handler_setup_stream, "application/x-apple-binary-plist", NULL, false },
    { AIRPLAY_SEQ_START_PLAYBACK, "POST /audioMode", EVRTSP_REQ_POST, payload_make_audio_mode, NULL, "application/x-apple-binary-plist", "/audioMode", false },
    // Some devices (e.g. Sonos Symfonisk) don't register the volume if it isn't last
    { AIRPLAY_SEQ_START_PLAYBACK, "SET_PARAMETER (volume)", EVRTSP_REQ_SET_PARAMETER, payload_make_set_volume, response_handler_volume_start, "text/parameters", NULL, true },
  },
  {
    { AIRPLAY_SEQ_PROBE, "GET /info (probe)", EVRTSP_REQ_GET, NULL, response_handler_info_probe, NULL, "/info", false },
  },
  {
    { AIRPLAY_SEQ_FLUSH, "FLUSH", EVRTSP_REQ_FLUSH, payload_make_flush, response_handler_flush, NULL, NULL, false },
  },
  {
    { AIRPLAY_SEQ_STOP, "TEARDOWN", EVRTSP_REQ_TEARDOWN, payload_make_teardown, response_handler_teardown, NULL, NULL, true },
  },
  {
    { AIRPLAY_SEQ_FAILURE, "TEARDOWN (failure)", EVRTSP_REQ_TEARDOWN, payload_make_teardown, response_handler_teardown_failure, NULL, NULL, false },
  },
  {
    { AIRPLAY_SEQ_PIN_START, "PIN start", EVRTSP_REQ_POST, payload_make_pin_start, response_handler_pin_start, NULL, "/pair-pin-start", false },
  },
  {
    { AIRPLAY_SEQ_SEND_VOLUME, "SET_PARAMETER (volume)", EVRTSP_REQ_SET_PARAMETER, payload_make_set_volume, NULL, "text/parameters", NULL, true },
  },
  {
    { AIRPLAY_SEQ_SEND_TEXT, "SET_PARAMETER (text)", EVRTSP_REQ_SET_PARAMETER, payload_make_send_text, NULL, "application/x-dmap-tagged", NULL, true },
  },
  {
    { AIRPLAY_SEQ_SEND_PROGRESS, "SET_PARAMETER (progress)", EVRTSP_REQ_SET_PARAMETER, payload_make_send_progress, NULL, "text/parameters", NULL, true },
  },
  {
    { AIRPLAY_SEQ_SEND_ARTWORK, "SET_PARAMETER (artwork)", EVRTSP_REQ_SET_PARAMETER, payload_make_send_artwork, NULL, NULL, NULL, true },
  },
  {
    { AIRPLAY_SEQ_PAIR_SETUP, "pair setup 1", EVRTSP_REQ_POST, payload_make_pair_setup1, response_handler_pair_setup1, "application/octet-stream", "/pair-setup", false },
    { AIRPLAY_SEQ_PAIR_SETUP, "pair setup 2", EVRTSP_REQ_POST, payload_make_pair_setup2, response_handler_pair_setup2, "application/octet-stream", "/pair-setup", false },
    { AIRPLAY_SEQ_PAIR_SETUP, "pair setup 3", EVRTSP_REQ_POST, payload_make_pair_setup3, response_handler_pair_setup3, "application/octet-stream", "/pair-setup", false },
  },
  {
    // Proceed on error is true because we want to delete the device key in the response handler if the verification fails
    { AIRPLAY_SEQ_PAIR_VERIFY, "pair verify 1", EVRTSP_REQ_POST, payload_make_pair_verify1, response_handler_pair_verify1, "application/octet-stream", "/pair-verify", true },
    { AIRPLAY_SEQ_PAIR_VERIFY, "pair verify 2", EVRTSP_REQ_POST, payload_make_pair_verify2, response_handler_pair_verify2, "application/octet-stream", "/pair-verify", false },
  },
  {
    // Some devices (i.e. my ATV4) gives a 470 when trying transient, so we proceed on that so the handler can trigger PIN setup sequence
    { AIRPLAY_SEQ_PAIR_TRANSIENT, "pair setup 1", EVRTSP_REQ_POST, payload_make_pair_setup1, response_handler_pair_setup1, "application/octet-stream", "/pair-setup", true },
    { AIRPLAY_SEQ_PAIR_TRANSIENT, "pair setup 2", EVRTSP_REQ_POST, payload_make_pair_setup2, response_handler_pair_setup2, "application/octet-stream", "/pair-setup", false },
  },
  {
    { AIRPLAY_SEQ_FEEDBACK, "POST /feedback", EVRTSP_REQ_POST, NULL, NULL, NULL, "/feedback", true },
  },
  {
    { AIRPLAY_SEQ_SEND_ANCHOR, "SETRATEANCHORTIME", EVRTSP_REQ_SETRATEANCHORTIME, payload_make_send_anchor, response_handler_send_anchor, "application/x-apple-binary-plist", NULL, true },
  },
  {
    { AIRPLAY_SEQ_FLUSH_BUFFERED, "FLUSHBUFFERED", EVRTSP_REQ_FLUSHBUFFERED, payload_make_flush_buffered, response_handler_flush_buffered, "application/x-apple-binary-plist", NULL, false },
  },
};


static void
sequence_continue_cb(struct evrtsp_request *req, void *arg)
{
  struct airplay_seq_ctx *seq_ctx = arg;
  struct airplay_seq_request *cur_request = seq_ctx->cur_request;
  struct airplay_session *session = seq_ctx->session;
  enum airplay_seq_type seq_type;

  session->reqs_in_flight--;
  if (!session->reqs_in_flight)
    evrtsp_connection_set_closecb(session->ctrl, rtsp_close_cb, session);

  if (!req)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "No response to %s from '%s'\n", cur_request->name, session->devname);
      // The keys have already worked on this connection, so this timeout
      // cannot be an auth rejection - do not cost the user their pairing.
      if (session->encrypted_ok)
	session->protocol_error = true;
      goto error;
    }

  if (req->response_code != RTSP_OK)
    {
      if (!cur_request->proceed_on_rtsp_not_ok)
	{
	  DPRINTF(E_LOG, L_AIRPLAY, "Response to %s from '%s' was negative, aborting (%d %s)\n", cur_request->name, session->devname, req->response_code, req->response_code_line);
	  goto error;
	}

      DPRINTF(E_WARN, L_AIRPLAY, "Response to %s from '%s' was negative, proceeding anyway (%d %s)\n", cur_request->name, session->devname, req->response_code, req->response_code_line);
    }

  // We don't check that the reply CSeq matches the request CSeq, because some
  // targets like Reflector and AirFoil don't return the CSeq according to the
  // rtsp spec. And the CSeq is not really important anyway.

  if (session->control_cipher_ctx)
    session->encrypted_ok = true;

  if (cur_request->response_handler)
    {
      seq_type = cur_request->response_handler(req, session);
      if (seq_type != AIRPLAY_SEQ_CONTINUE)
	{
	  if (seq_type == AIRPLAY_SEQ_ABORT)
	    goto error;

	  // Handler wanted to start a new sequence
	  sequence_start(seq_type, seq_ctx->session, seq_ctx->payload_make_arg, seq_ctx->log_caller);
	  free(seq_ctx);
	  return;
	}
    }

  seq_ctx->cur_request++;

  if (seq_ctx->cur_request->name)
    {
      sequence_continue(seq_ctx);
      return;
    }

  if (seq_ctx->on_success)
    seq_ctx->on_success(session);

  free(seq_ctx);
  return;

 error:
  if (seq_ctx->on_error)
    seq_ctx->on_error(session);

  free(seq_ctx);
}

static void
sequence_continue(struct airplay_seq_ctx *seq_ctx)
{
  struct airplay_session *session = seq_ctx->session;
  struct airplay_seq_request *cur_request = seq_ctx->cur_request;
  struct evrtsp_request *req = NULL;
  const char *uri;
  int ret;

  req = evrtsp_request_new(sequence_continue_cb, seq_ctx);
  if (!req)
    goto error;

  ret = request_headers_add(req, session, cur_request->rtsp_type);
  if (ret < 0)
    goto error;

  if (cur_request->content_type)
    evrtsp_add_header(req->output_headers, "Content-Type", cur_request->content_type);

  if (cur_request->payload_make)
    {
      ret = cur_request->payload_make(req, session, seq_ctx->payload_make_arg);
      if (ret > 0) // Skip to next request in sequence, if none -> error
        {
	  seq_ctx->cur_request++;
	  if (!seq_ctx->cur_request->name)
	    {
	      DPRINTF(E_LOG, L_AIRPLAY, "Bug! payload_make signaled skip request, but there is nothing to skip to\n");
	      goto error;
	    }

	  evrtsp_request_free(req);
	  sequence_continue(seq_ctx);
	  return;
        }
      else if (ret < 0)
	goto error;
    }

  uri = (cur_request->uri) ? cur_request->uri : session->session_url;

  DPRINTF(E_DBG, L_AIRPLAY, "%s: Sending %s to '%s'\n", seq_ctx->log_caller, cur_request->name, session->devname);

  ret = evrtsp_make_request(session->ctrl, req, cur_request->rtsp_type, uri);
  if (ret < 0)
    goto error;

  evrtsp_connection_set_closecb(session->ctrl, NULL, NULL);

  session->reqs_in_flight++;

  return;

 error:
  DPRINTF(E_LOG, L_AIRPLAY, "%s: Error sending %s to '%s'\n", seq_ctx->log_caller, cur_request->name, session->devname);

  if (req)
    evrtsp_request_free(req);

  // Sets status to FAILED, gives status to player and frees session. Must be
  // deferred, otherwise sequence_start() could invalidate the session, meaning
  // any dereference of the session by the caller after sequence_start() would
  // segfault.
  deferred_session_failure(session);

  free(seq_ctx);
}

// All errors that may occur during a sequence are called back async
static void
sequence_start(enum airplay_seq_type seq_type, struct airplay_session *session, void *arg, const char *log_caller)
{
  struct airplay_seq_ctx *seq_ctx;

  CHECK_NULL(L_AIRPLAY, seq_ctx = calloc(1, sizeof(struct airplay_seq_ctx)));

  seq_ctx->session = session;
  seq_ctx->cur_request = &airplay_seq_request[seq_type][0]; // First step of the sequence
  seq_ctx->on_success = airplay_seq_definition[seq_type].on_success;
  seq_ctx->on_error = airplay_seq_definition[seq_type].on_error;
  seq_ctx->payload_make_arg = arg;
  seq_ctx->log_caller = log_caller;

  sequence_continue(seq_ctx); // Ownership transferred
}


/* ---------------- Airplay devices discovery - mDNS callback --------------- */
/*                              Thread: main (mdns)                           */

static int
features_parse(struct keyval *features_kv, const char *features_txt, const char *name)
{
  uint64_t features = 0;
  const char *delim_ptr;
  int i, j;

  // Even though features_txt may be two commaseparated values we can pass it to
  // safe_hextou32() which will only convert the first value.
  if ( safe_hextou32(features_txt, (uint32_t *)&features) < 0 ||
       ((delim_ptr = strchr(features_txt, ',')) && safe_hextou32(delim_ptr + 1, ((uint32_t *)&features) + 1) < 0) )
    {
      DPRINTF(E_LOG, L_AIRPLAY, "AirPlay '%s': unexpected features field '%s' in TXT record\n", name, features_txt);
      return -1;
    }

  // Walk through the bits
  for (i = 0; i < (sizeof(features) * CHAR_BIT); i++)
    {
      if (((features >> i) & 0x01) == 0)
        continue;

      // Check if we have it in the features map
      for (j = 0; j < ARRAY_SIZE(features_map); j++)
	{
	  if (i == features_map[j].bit)
	    {
	      DPRINTF(E_SPAM, L_AIRPLAY, "Speaker '%s' announced feature %d: '%s'\n", name, i, features_map[j].name);
              keyval_add(features_kv, features_map[j].name, "1");
	      break;
	    }
	}

      if (j == ARRAY_SIZE(features_map))
	DPRINTF(E_SPAM, L_AIRPLAY, "Speaker '%s' announced feature %d: 'Unknown'\n", name, i);
    }

  return 0;
}

static bool
txt_flag_is_true(const char *value)
{
  if (!value)
    return false;

  return (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0);
}

static bool
airplay_name_contains_tv(const char *name)
{
  const char *p;

  if (!name)
    return false;

  for (p = name; *p; p++)
    {
      if (strncasecmp(p, "TV", 2) == 0)
        return true;
    }

  return false;
}

static bool
airplay_is_appletv_device(enum airplay_devtype devtype)
{
  return (devtype == AIRPLAY_DEV_APPLETV || devtype == AIRPLAY_DEV_APPLETV4);
}

// See the forward declaration/comment near the top of this file for the
// rationale: surround is only offered/accepted for a standalone Apple TV,
// not a HomePod, not a stereo-pair member, and not the visible leader of a
// HomePod-behind-Apple-TV proxy group.
static bool
airplay_device_allows_surround(struct output_device *device)
{
  struct airplay_extra *extra;

  if (!device || !device->extra_device_info)
    return false;

  extra = device->extra_device_info;

  if (!airplay_is_appletv_device(extra->devtype))
    return false;

  if (outputs_device_is_tv_proxy_group(device))
    return false;

  if (device->is_grouped)
    return false;

  return true;
}

static bool
airplay_homepod_gid_suffix_is(const char *gid, const char *tsid, const char *suffix)
{
  size_t tsid_len;

  if (!gid || !tsid || !suffix)
    return false;

  tsid_len = strlen(tsid);

  return (strncmp(gid, tsid, tsid_len) == 0 && strcmp(gid + tsid_len, suffix) == 0);
}

static char *
airplay_stereo_group_id_normalize(struct airplay_extra *extra)
{
  // HomePods may advertise a shared pair gid initially and later re-advertise
  // per-member gids such as "<tsid>+0" and "<tsid>+1". The tsid has been the
  // stable pair identifier across those transitions, so prefer it as the
  // runtime grouping key when present.
  if (extra->devtype == AIRPLAY_DEV_HOMEPOD && extra->tsid)
    return safe_strdup(extra->tsid);

  return safe_strdup(extra->gid);
}

static bool
airplay_stereo_leader_is_visible(struct airplay_extra *extra)
{
  if (!extra->igl)
    return false;

  // When HomePods split into per-member gids, both members may advertise
  // igl=1. Observed advertisements use "<tsid>+0" for the visible leader and
  // "<tsid>+1" for the secondary member.
  if (extra->devtype == AIRPLAY_DEV_HOMEPOD && extra->gid && extra->tsid)
    {
      if (airplay_homepod_gid_suffix_is(extra->gid, extra->tsid, "+1"))
        return false;

      if (airplay_homepod_gid_suffix_is(extra->gid, extra->tsid, "+0"))
        return true;
    }

  return true;
}

static void
airplay_stereo_state_set(struct output_device *device, struct airplay_extra *extra)
{
  bool grouped;
  char *group_id;

  // Based on observed HomePod stereo-pair advertisements we only treat a
  // speaker as stereo-grouped when:
  //  - it is a HomePod-class device
  //  - gid is present
  //  - gpn is present (pair name in Home)
  // The runtime group key is normalized separately because HomePods may switch
  // between a shared pair gid and per-member gids while still representing
  // the same stereo pair.
  grouped = (extra->devtype == AIRPLAY_DEV_HOMEPOD && extra->gid && extra->gpn);

  device->leader_hint_gcgl = extra->gcgl;
  device->leader_hint_igl = extra->igl;

  // Persist the raw mDNS gid for cross-referencing Apple TVs with their proxied
  // HomePod groups in output_group_refresh(). Only set when the device
  // advertises a gid (AirPlay 2 devices); RAOP-only devices have no gid.
  if (extra->gid)
    {
      free(device->raw_gid);
      device->raw_gid = safe_strdup(extra->gid);
    }

  if (!grouped)
    {
      if (airplay_is_appletv_device(extra->devtype) && !airplay_name_contains_tv(device->name))
        device->display_name = safe_asprintf("%s (TV)", device->name);
      else
        device->display_name = safe_strdup(device->name);

      return;
    }

  group_id = airplay_stereo_group_id_normalize(extra);

  device->group_id = group_id;
  device->group_name = safe_strdup(extra->gpn);
  device->is_grouped = 1;
  device->is_group_leader = airplay_stereo_leader_is_visible(extra);
  device->is_group_hidden = !device->is_group_leader;
  device->group_leader_id = device->is_group_leader ? device->id : 0;

  if (device->is_group_leader)
    device->display_name = safe_asprintf("%s (Stereo)", extra->gpn);
  else
    device->display_name = safe_strdup(device->name);
}

static void
airplay_stereo_metadata_log(struct output_device *device, struct airplay_extra *extra)
{
  const char *gid = extra->gid ? extra->gid : "(null)";
  const char *gpn = extra->gpn ? extra->gpn : "(null)";
  const char *tsid = extra->tsid ? extra->tsid : "(null)";
  const char *group_id = device->group_id ? device->group_id : "(null)";
  const char *display_name = device->display_name ? device->display_name : device->name;

  DPRINTF(E_DBG, L_AIRPLAY, "AirPlay device '%s': stereo metadata gid=%s, gpn='%s', tsid=%s, gcgl=%u, igl=%u\n",
          device->name, gid, gpn, tsid, extra->gcgl, extra->igl);

  DPRINTF(E_DBG, L_AIRPLAY,
          "AirPlay device '%s': derived stereo state type=%s, raw gid=%s, normalized group_id=%s, gpn='%s', tsid=%s, gcgl=%u, igl=%u, is_grouped=%u, is_group_leader=%u, is_group_hidden=%u, display_name='%s'\n",
          device->name, airplay_devtype[extra->devtype], gid, group_id, gpn, tsid,
          extra->gcgl, extra->igl, device->is_grouped, device->is_group_leader,
          device->is_group_hidden, display_name);

  if ((extra->gcgl || extra->igl) && !extra->gid)
    DPRINTF(E_WARN, L_AIRPLAY, "AirPlay stereo metadata inconsistency for device '%s': leader flags set but gid missing\n",
            device->name);

  if (!extra->gid)
    {
      DPRINTF(E_DBG, L_AIRPLAY, "AirPlay device '%s': no stereo metadata present\n", device->name);
      return;
    }

  if (!device->is_grouped)
    {
      DPRINTF(E_DBG, L_AIRPLAY, "AirPlay device '%s': gid present but not classified as stereo pair (type=%s, gpn='%s')\n",
              device->name, airplay_devtype[extra->devtype], gpn);
      return;
    }

  DPRINTF(E_INFO, L_AIRPLAY, "AirPlay stereo group detected for device '%s': group id=%s, raw gid=%s, group name='%s'\n",
          device->name, group_id, gid, gpn);

  if (device->is_group_leader)
    DPRINTF(E_INFO, L_AIRPLAY, "AirPlay stereo leader detected: device '%s', group id=%s, raw gid=%s, group name='%s'\n",
            device->name, group_id, gid, gpn);
  else
    DPRINTF(E_DBG, L_AIRPLAY, "AirPlay device '%s': classified as stereo member, leader=0\n", device->name);
}


/* Examples of txt content:
 * Airport Express 2:
     ["pk=7de...39" "gcgl=0" "gid=0fd...4" "pi=0fd...a4" "srcvers=366.0" "protovers=1.1" "serialNumber=C8...R" "manufacturer=Apple Inc." "model=AirPort10,115" "flags=0x4" "fv=p20.78100.3" "rsf=0x0" "features=0x445D0A00,0x1C340" "deviceid=74:1B:B2:D1:1A:B7" "acl=0"]
 * Apple TV 4:
     ["vv=2" "osvers=14.2" "srcvers=525.38.42" "pk=c4e...c88" "psi=67C...DBC" "pi=b0b...da0" "protovers=1.1" "model=AppleTV5,3" "gcgl=1" "igl=1" "gid=B...73" "flags=0x244" "features=0x5A7FDFD5,0x3C155FDE" "fex=1d9/Wt5fFTw" "deviceid=AA:BB:CC:DD:EE:FF" "btaddr=D0:00:44:66:BB:66" "acl=0"]
  * Roku
     ["pk=xxxxxxxxx” "gcgl=0" "gid=xxxxxxx” "psi=xxxxx” "pi=8A:71:CA:EF:xxxx" "srcvers=377.28.01" "protovers=1.1" "serialNumber=xxxxxxx” "manufacturer=Roku" "model=3810X" "flags=0x644" "at=0x3" "fv=p20.9.40.4190" "rsf=0x3" "features=0x7F8AD0,0x10BCF46" "deviceid=8A:71:CA:xxxxx” "acl=0"]
  * Samsung TV
     ["pk=7xxxxxxxxxx” "gcgl=0" "gid=xxxxxxxxxxx” "psi=xxxxxxx” "pi=4C:6F:64:xxxxxxx” "srcvers=377.17.24.6" "protovers=1.1" "serialNumber=xxxxxxx” "manufacturer=Samsung" "model=UNU7090" "flags=0x244" "fv=p20.0.1" "rsf=0x3" "features=0x7F8AD0,0x38BCB46" "deviceid=64:1C:AE:xxxxx” "acl=0"]
 * HomePod
     ["vv=2" "osvers=14.3" "srcvers=530.6" "pk=..." "psi=31...D3" "pi=fd...87" "protovers=1.1" "model=AudioAccessory1,1" "tsid=4...E" "gpn=name" "gcgl=1" "igl=1" "gid=4...E" "flags=0x1a404" "features=0x4A7FCA00,0x3C356BD0" "fex=AMp/StBrNTw" "deviceid=D4:...:C1" "btaddr=5E:...:F1" "acl=0"]
 *
 * Observed HomePod stereo-pair behavior in the field:
 *  - both members may initially share the same gid and gpn
 *  - later advertisements may split gid into per-member values such as
 *    "<tsid>+0" and "<tsid>+1"
 *  - both may advertise gcgl=1
 *  - igl=1 appears to identify the visible leader more reliably
 *  - when the gid splits per member, tsid remains the stable pair identifier
 *  - a "<tsid>+0" gid has identified the visible leader more reliably than
 *    igl alone in those split-gid advertisements
 * Sonos Symfonisk
     ["pk=e5...1c" "gcgl=0" "gid=[uuid]" "pi=[uuid]" "srcvers=366.0" "protovers=1.1" "serialNumber=xx" "manufacturer=Sonos" "model=Bookshelf" "flags=0x4" "fv=p20.63.2-88230" "rsf=0x0" "features=0x445F8A00,0x1C340" "deviceid=11:22:33:44:55:66" "acl=0"]
  * Marantz NR1607 and SR6009 (which don't support pairing)
     ["srcvers=190.9.p6" "model=NR1607" "fv=p105321.1400.0" "flags=0x4" "features=0x444F8A00" "deviceid=00:06:12:12:12:12"]
     ["srcvers=190.9" "seed=99" "model=SR6009" "flags=0x4" "features=0x444F8A00" "deviceid=00:06:12:12:12:12"]
  * Apple TV 3 (pairing supported, but not encryption)
     ["vv=2" "srcvers=220.68" "pi=24...08" "pk=4b...48" "pw=1" "model=AppleTV3,1" "flags=0xc4" "features=0x5A7FFFF7,0xE" "deviceid=70:73:12:12:12:12"]
  * Libratone Loop
     ["srcvers=190.9" "seed=99" "model=LibratoneLoop1" "flags=0x4" "features=0x444C0A00" "deviceid=XX:XX:XX:XX:XX:XX"]
 */
static void
airplay_device_cb(const char *name, const char *type, const char *domain, const char *hostname, int family, const char *address, int port, struct keyval *txt)
{
  struct output_device *device;
  struct airplay_extra *extra;
  struct keyval features_kv = { 0 };
  cfg_t *devcfg;
  cfg_opt_t *cfgopt;
  const char *p;
  const char *nickname = NULL;
  const char *password = NULL;
  const char *features;
  uint64_t id;
  int ret;

  if (port > 0)
    {
      p = keyval_get(txt, "deviceid");
      if (!p)
	{
	  DPRINTF(E_LOG, L_AIRPLAY, "AirPlay device '%s' is missing a device ID\n", name);
	  return;
	}

      ret = device_id_colon_parse(&id, p);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_AIRPLAY, "Could not extract AirPlay device ID ('%s'): %s\n", name, p);
	  return;
	}
    }
  else
    {
      ret = device_id_find_byname(&id, name);
      if (ret < 0)
	{
	  DPRINTF(E_WARN, L_AIRPLAY, "Could not remove, AirPlay device '%s' not in our list\n", name);
	  return;
	}
    }

  DPRINTF(E_DBG, L_AIRPLAY, "Event for AirPlay device '%s' (port %d, id %" PRIx64 ", Active-Remote %" PRIu32 ")\n", name, port, id, (uint32_t)id);

  devcfg = cfg_gettsec(cfg, "airplay", name);
  if (devcfg && cfg_getbool(devcfg, "exclude"))
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Excluding AirPlay device '%s' as set in config\n", name);
      return;
    }
  if (devcfg && cfg_getbool(devcfg, "permanent") && (port < 0))
    {
      DPRINTF(E_INFO, L_AIRPLAY, "AirPlay device '%s' disappeared, but set as permanent in config\n", name);
      return;
    }
  if (outputs_exclusive_mode_get() && !(devcfg && cfg_getbool(devcfg, "exclusive")))
    {
      DPRINTF(E_INFO, L_AIRPLAY, "AirPlay device '%s' ignored, other speaker(s) set as exclusive\n", name);
      return;
    }
  if (devcfg && cfg_getbool(devcfg, "airplay2_disable"))
    {
      DPRINTF(E_INFO, L_AIRPLAY, "Disabling AirPlay 2 for device '%s' as set in config\n", name);
      return;
    }
  if (devcfg && cfg_getstr(devcfg, "nickname"))
    {
      nickname = cfg_getstr(devcfg, "nickname");
    }
  if (devcfg && cfg_getstr(devcfg, "password"))
    {
      password = cfg_getstr(devcfg, "password");
    }

  CHECK_NULL(L_AIRPLAY, device = calloc(1, sizeof(struct output_device)));
  CHECK_NULL(L_AIRPLAY, extra = calloc(1, sizeof(struct airplay_extra)));

  device->id = id;
  device->name = nickname ? strdup(nickname) : strdup(name);
  device->password = password;
  device->type = OUTPUT_TYPE_AIRPLAY;
  device->type_name = outputs_name(device->type);
  device->extra_device_info = extra;
  device->supported_formats = MEDIA_FORMAT_ALAC;

  extra->mdns_name = strdup(name); // Used for identifying device when it disappears

  if (port < 0)
    {
      // Device stopped advertising
      switch (family)
	{
	  case AF_INET:
	    device->v4_port = 1;
	    break;

	  case AF_INET6:
	    device->v6_port = 1;
	    break;
	}

      ret = player_device_remove(device);
      if (ret < 0)
	goto free_device;

      return;
    }

  // Features, see features_map[]
  features = keyval_get(txt, "features");
  if (!features)
    {
      DPRINTF(E_WARN, L_AIRPLAY, "Not using AirPlay 2 for device '%s' as it does not have required 'features' in TXT field\n", name);
      goto free_device;
    }

  ret = features_parse(&features_kv, features, name);
  if (ret < 0)
    goto free_device;

  if (!keyval_get(&features_kv, "SupportsAirPlayAudio"))
    {
      DPRINTF(E_DBG, L_AIRPLAY, "AirPlay device '%s' does not support audio\n", name);
      goto free_device;
    }
  if (!keyval_get(&features_kv, "SupportsCoreUtilsPairingAndEncryption"))
    {
      DPRINTF(E_DBG, L_AIRPLAY, "AirPlay device '%s' does not support pairing/encryption, falling back to RAOP\n", name);
      goto free_device;
    }

  if (keyval_get(&features_kv, "MetadataFeatures_0"))
    extra->wanted_metadata |= AIRPLAY_MD_WANTS_ARTWORK;
  if (keyval_get(&features_kv, "MetadataFeatures_1"))
    extra->wanted_metadata |= AIRPLAY_MD_WANTS_PROGRESS;
  if (keyval_get(&features_kv, "MetadataFeatures_2"))
    extra->wanted_metadata |= AIRPLAY_MD_WANTS_TEXT;
  if (keyval_get(&features_kv, "Authentication_8"))
    extra->supports_auth_setup = 1;
  if (keyval_get(&features_kv, "SupportsPTP") && !(devcfg && cfg_getbool(devcfg, "ptp_disable")) && !airplay_ptp_is_disabled)
    extra->use_ptp = 1;

  // Currently always true since no SupportsCoreUtilsPairingAndEncryption means we fall back to RAOP
  extra->supports_encryption = 1;

  keyval_clear(&features_kv);

  // Only default audio quality supported so far
  device->quality.sample_rate = AIRPLAY_QUALITY_SAMPLE_RATE_DEFAULT;
  device->quality.bits_per_sample = AIRPLAY_QUALITY_BITS_PER_SAMPLE_DEFAULT;
  device->quality.channels = AIRPLAY_QUALITY_CHANNELS_DEFAULT;

  if (!quality_is_equal(&device->quality, &airplay_quality_default))
    DPRINTF(E_INFO, L_AIRPLAY, "Device '%s' requested non-default audio quality (%d/%d/%d)\n",
                                device->name, device->quality.sample_rate, device->quality.bits_per_sample, device->quality.channels);

  // Device type
  extra->devtype = AIRPLAY_DEV_OTHER;
  p = keyval_get(txt, "model");

  if (!p)
    extra->devtype = AIRPLAY_DEV_OTHER;
  else if (strncmp(p, "AirPort4", strlen("AirPort4")) == 0)
    extra->devtype = AIRPLAY_DEV_APEX2_80211N; // Second generation
  else if (strncmp(p, "AirPort", strlen("AirPort")) == 0)
    extra->devtype = AIRPLAY_DEV_APEX3_80211N; // Third generation and newer
  else if (strncmp(p, "AppleTV5,3", strlen("AppleTV5,3")) == 0)
    extra->devtype = AIRPLAY_DEV_APPLETV4; // Stream to ATV with tvOS 10 needs to be kept alive
  else if (strncmp(p, "AppleTV", strlen("AppleTV")) == 0)
    extra->devtype = AIRPLAY_DEV_APPLETV;
  else if (strncmp(p, "AudioAccessory", strlen("AudioAccessory")) == 0)
    extra->devtype = AIRPLAY_DEV_HOMEPOD;
  else if (*p == '\0')
    DPRINTF(E_WARN, L_AIRPLAY, "AirPlay device '%s': am has no value\n", name);

  extra->gid = safe_strdup(keyval_get(txt, "gid"));
  extra->gpn = safe_strdup(keyval_get(txt, "gpn"));
  extra->tsid = safe_strdup(keyval_get(txt, "tsid"));
  extra->gcgl = txt_flag_is_true(keyval_get(txt, "gcgl"));
  extra->igl = txt_flag_is_true(keyval_get(txt, "igl"));

  airplay_stereo_state_set(device, extra);

  airplay_stereo_metadata_log(device, extra);

  // If the user didn't set any reconnect setting we enable for Apple TV and
  // HomePods due to https://github.com/owntone/owntone-server/issues/734
  cfgopt = devcfg ? cfg_getopt(devcfg, "reconnect") : NULL;
  if (cfgopt && cfgopt->nvalues == 1)
    device->resurrect = cfg_opt_getnbool(cfgopt, 0);
  else
    device->resurrect = (extra->devtype == AIRPLAY_DEV_APPLETV4) || (extra->devtype == AIRPLAY_DEV_HOMEPOD);

  switch (family)
    {
      case AF_INET:
	device->v4_address = strdup(address);
	device->v4_port = port;
	DPRINTF(E_INFO, L_AIRPLAY, "Adding AirPlay device '%s': features %s, type %s, address %s:%d\n", 
	  name, features, airplay_devtype[extra->devtype], address, port);
	break;

      case AF_INET6:
	device->v6_address = strdup(address);
	device->v6_port = port;
	DPRINTF(E_INFO, L_AIRPLAY, "Adding AirPlay device '%s': features %s, type %s, address [%s]:%d\n", 
	  name, features, airplay_devtype[extra->devtype], address, port);
	break;

      default:
	DPRINTF(E_LOG, L_AIRPLAY, "Error: AirPlay device '%s' has neither ipv4 og ipv6 address\n", name);
	goto free_device;
    }

  ret = player_device_add(device);
  if (ret < 0)
    goto free_device;

  return;

 free_device:
  outputs_device_free(device);
  keyval_clear(&features_kv);
}


/* ---------------------------- Module definitions -------------------------- */
/*                                Thread: player                              */

static int
airplay_device_probe(struct output_device *device, int callback_id)
{
  struct airplay_session *session;

  session = session_make(device, callback_id);
  if (!session)
    return -1;

  session->probing = true;

  sequence_start(AIRPLAY_SEQ_PROBE, session, NULL, "device_probe");

  return 1;
}

static int
airplay_device_start(struct output_device *device, int callback_id)
{
  struct airplay_session *session;

  session = session_make(device, callback_id);
  if (!session)
    return -1;

  sequence_start(AIRPLAY_SEQ_START, session, NULL, "device_start");

  return 1;
}

static int
airplay_device_stop(struct output_device *device, int callback_id)
{
  struct airplay_session *session = device->session;

  session->callback_id = callback_id;

  sequence_start(AIRPLAY_SEQ_STOP, session, NULL, "device_stop");

  return 1;
}

static int
airplay_device_flush(struct output_device *device, int callback_id)
{
  struct airplay_session *session = device->session;

  if (session->state != AIRPLAY_STATE_STREAMING)
    return 0; // No-op, nothing to flush

  session->callback_id = callback_id;

  sequence_start(session->buffered_mode ? AIRPLAY_SEQ_FLUSH_BUFFERED : AIRPLAY_SEQ_FLUSH, session, NULL, "flush");

  return 1;
}

static void
airplay_device_cb_set(struct output_device *device, int callback_id)
{
  struct airplay_session *session = device->session;

  session->callback_id = callback_id;
}

static void
airplay_device_free_extra(struct output_device *device)
{
  struct airplay_extra *extra = device->extra_device_info;

  free(extra->mdns_name);
  free(extra->gid);
  free(extra->gpn);
  free(extra->tsid);
  free(extra);
}

static int
airplay_device_authorize(struct output_device *device, const char *pin, int callback_id)
{
  struct airplay_session *session;

  // Make a session so we can communicate with the device
  session = session_make(device, callback_id);
  if (!session)
    return -1;

  sequence_start(AIRPLAY_SEQ_PAIR_SETUP, session, (void *)pin, "device_authorize");

  return 1;
}

static void
airplay_write(struct output_buffer *obuf)
{
  struct airplay_master_session *ams;
  struct airplay_session *session;
  struct airplay_encoded_frame *frame, *next_frame;
  int i;

  for (ams = airplay_master_sessions; ams; ams = ams->next)
    {
      // Buffered ams: deliver whatever the encoder thread has finished since
      // the last tick before feeding it more PCM, so frames leave in the
      // order they were encoded.
      if (ams->encoder)
	{
	  frame = airplay_encoder_frames_get(ams->encoder);
	  while (frame)
	    {
	      next_frame = frame->next;
	      buffered_frame_send(ams, frame->data, frame->len);
	      airplay_encoder_frame_free(frame);
	      frame = next_frame;
	    }

	  if (airplay_encoder_failed(ams->encoder))
	    {
	      DPRINTF(E_LOG, L_AIRPLAY, "Buffered encoder (kind %d) failed, ending its sessions\n", ams->buffered_kind);

	      for (session = airplay_sessions; session; session = session->next)
		{
		  if (session->master_session != ams)
		    continue;
		  if (session->state != AIRPLAY_STATE_CONNECTED && session->state != AIRPLAY_STATE_STREAMING)
		    continue;

		  deferred_session_failure(session);
		}
	    }
	}

      for (i = 0; obuf->data[i].buffer; i++)
	{
	  if (!quality_is_equal(&obuf->data[i].quality, &ams->rtp_session->quality))
	    continue;

	  // Set ams->cur_stamp, which involves a calculation of which session
	  // rtptime corresponds to the pts we are given by the player.
	  timestamp_set(ams, obuf->pts);

	  // Sends sync packets to new sessions, and if it is sync time then also
	  // to old sessions. Buffered sessions have no UDP sync/retransmit
	  // buffer, so this is realtime-only.
	  if (!ams->buffered)
	    packets_sync_send(ams);

	  if (ams->encoder)
	    {
	      // Copy - obuf is drained/reused by the player right after this
	      // function returns.
	      airplay_encoder_pcm_write(ams->encoder, obuf->data[i].buffer, obuf->data[i].bufsize, obuf->data[i].samples);
	      continue;
	    }

	  // TODO avoid this copy
	  evbuffer_add(ams->input_buffer, obuf->data[i].buffer, obuf->data[i].bufsize);
	  ams->input_buffer_samples += obuf->data[i].samples;

	  // Send as many packets as we have data for (one packet requires rawbuf_size bytes)
	  while (evbuffer_get_length(ams->input_buffer) >= ams->rawbuf_size)
	    {
	      evbuffer_remove(ams->input_buffer, ams->rawbuf, ams->rawbuf_size);
	      ams->input_buffer_samples -= ams->samples_per_packet;

	      packets_send(ams);
	    }
	}
    }

  // Check for devices that have joined since last write (we have already sent them
  // initialization sync and rtp packets via packets_sync_send and packets_send)
  for (session = airplay_sessions; session; session = session->next)
    {
      if (session->state == AIRPLAY_STATE_CONNECTED)
	{
	  // Start sending progress to keep ATV's alive
	  if (!event_pending(keep_alive_timer, EV_TIMEOUT, NULL))
	    evtimer_add(keep_alive_timer, &keep_alive_tv);

	  session->state = AIRPLAY_STATE_STREAMING;
	}

      // Buffered receivers don't start playing until they've received a
      // rate=1 SETRATEANCHORTIME, and audio is held until the receiver
      // accepts it (see the anchor_confirmed gate in buffered_frame_send) so
      // the first delivered frame carries exactly the anchor's rtpTime.
      // anchor_pending is set when buffered_mode is first decided and again
      // after every FLUSHBUFFERED, which invalidates the receiver's previous
      // anchor.
      if (session->state == AIRPLAY_STATE_STREAMING && session->buffered_mode && session->anchor_pending)
	{
	  sequence_start(AIRPLAY_SEQ_SEND_ANCHOR, session, NULL, "anchor");
	  session->anchor_pending = false;
	}
      // Make a cb?
    }
}

// Scores the host's encode capacity so the admission budget (ENCODER_COST_*)
// scales with the board instead of assuming every core is equally fast. Core
// count alone is wrong across the fleet: a clock-limited board with cores
// offlined must not get the same budget as a stock one at full clock, and a
// much faster board should be effectively uncapped.
static int
encoder_budget_compute(void)
{
  long online_cores;
  long conf_cores;
  double max_khz = 0.0;
  double max_clock_ghz;
  double points;
  int arch_weight;
  int cpu;
  bool have_clock = false;
  struct utsname uts;
  int budget;
  bool used_fallback = false;
  char path[96];
  FILE *fp;
  long khz;

  online_cores = sysconf(_SC_NPROCESSORS_ONLN);
  if (online_cores < 1)
    online_cores = 1;

  conf_cores = sysconf(_SC_NPROCESSORS_CONF);
  if (conf_cores < online_cores)
    conf_cores = online_cores;

  if (uname(&uts) != 0)
    snprintf(uts.machine, sizeof(uts.machine), "unknown");

  // Kernel arch, deliberately: a 32-bit userland on a 64-bit kernel still
  // scores the silicon, not the ABI.
  if (strncmp(uts.machine, "armv6", 5) == 0)
    arch_weight = 1;
  else if (strncmp(uts.machine, "armv7", 5) == 0)
    arch_weight = 2;
  else if (strcmp(uts.machine, "aarch64") == 0)
    arch_weight = 4;
  else
    arch_weight = 4;

  for (cpu = 0; cpu < conf_cores; cpu++)
    {
      snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
      fp = fopen(path, "r");
      if (!fp)
        continue; // offline or not present on this kernel

      if (fscanf(fp, "%ld", &khz) == 1 && khz > max_khz)
        {
          have_clock = true;
          max_khz = khz;
        }
      fclose(fp);
    }

  if (have_clock)
    {
      max_clock_ghz = max_khz / 1e6;
      points = arch_weight * max_clock_ghz * online_cores;
      budget = (int)lround(0.75 * points);
      if (budget < 2)
        budget = 2;
      else if (budget > 64)
        budget = 64;
    }
  else
    {
      // No boot-time clock limit readable (WSL/VMs/some x86): reserve one
      // core for the player/RTSP thread and score the rest at the default cost.
      used_fallback = true;
      max_clock_ghz = 0.0;
      points = 0.0;
      budget = 4 * ((online_cores - 1 > 1) ? (online_cores - 1) : 1);
    }

  DPRINTF(E_INFO, L_AIRPLAY, "Encoder admission budget: machine '%s', %ld online cores, max clock %.2f GHz, points %.2f -> budget %d%s\n",
          uts.machine, online_cores, max_clock_ghz, points, budget, used_fallback ? " (fallback)" : "");

  return budget;
}

static int
airplay_init(void)
{
  int ret;
  int i;
  int timing_port;
  int control_port;

  airplay_device_id = libhash;

  encoder_cost_budget = encoder_budget_compute();

  uuid_make(airplay_ptp_clock_uuid);

  // Check alignment of enum seq_type with airplay_seq_definition and
  // airplay_seq_request
  for (i = 0; i < ARRAY_SIZE(airplay_seq_definition); i++)
    {
      if (airplay_seq_definition[i].seq_type != i || airplay_seq_request[i][0].seq_type != i)
        {
	  DPRINTF(E_LOG, L_AIRPLAY, "Bug! Misalignment between sequence enum and structs: %d, %d, %d\n", i, airplay_seq_definition[i].seq_type, airplay_seq_request[i][0].seq_type);
	  return -1;
        }
    }

  CHECK_NULL(L_AIRPLAY, keep_alive_timer = evtimer_new(evbase_player, airplay_keep_alive_timer_cb, NULL));

  airplay_user_agent = cfg_getstr(cfg_getsec(cfg, "general"), "user_agent");
  airplay_client_name = cfg_getstr(cfg_getsec(cfg, "library"), "name");

  timing_port = cfg_getint(cfg_getsec(cfg, "airplay_shared"), "timing_port");
  ret = service_start(&airplay_timing_svc, timing_svc_cb, timing_port, "AirPlay timing");
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "AirPlay time synchronization failed to start\n");
      goto out_free_timer;
    }

  control_port = cfg_getint(cfg_getsec(cfg, "airplay_shared"), "control_port");
  ret = service_start(&airplay_control_svc, control_svc_cb, control_port, "AirPlay control");
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "AirPlay playback control failed to start\n");
      goto out_stop_timing;
    }

  ret = airplay_events_init();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "AirPlay events failed to start\n");
      goto out_stop_control;
    }

  // libhash is just a seed to make the clock id unique
  ret = ptpd_init(libhash);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "AirPlay PTP daemon unavailable, only NTP will be available\n");
      airplay_ptp_is_disabled = true;
    }

  ret = mdns_browse("_airplay._tcp", airplay_device_cb, MDNS_CONNECTION_TEST);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not add mDNS browser for AirPlay devices\n");
      goto out_stop_events;
    }

  return 0;

 out_stop_events:
  airplay_events_deinit();
 out_stop_control:
  service_stop(&airplay_control_svc);
 out_stop_timing:
  service_stop(&airplay_timing_svc);
 out_free_timer:
  event_free(keep_alive_timer);

  return -1;
}

static void
airplay_deinit(void)
{
  struct airplay_session *session;

  airplay_events_deinit();
  service_stop(&airplay_control_svc);
  service_stop(&airplay_timing_svc);

  event_free(keep_alive_timer);

  for (session = airplay_sessions; airplay_sessions; session = airplay_sessions)
    {
      airplay_sessions = session->next;

      session_free(session);
    }

  // After freeing sessions, since that's where the active ptp peers get removed
  ptpd_deinit();
}

struct output_definition output_airplay =
{
  .name = "AirPlay 2",
  .cfg_name = "airplay",
  .type = OUTPUT_TYPE_AIRPLAY,
#ifdef PREFER_AIRPLAY2
  .priority = 1,
#else
  .priority = 2,
#endif
  .disabled = 0,
  .init = airplay_init,
  .deinit = airplay_deinit,
  .device_start = airplay_device_start,
  .device_stop = airplay_device_stop,
  .device_flush = airplay_device_flush,
  .device_probe = airplay_device_probe,
  .device_cb_set = airplay_device_cb_set,
  .device_free_extra = airplay_device_free_extra,
  .device_volume_set = airplay_set_volume_one,
  .device_volume_to_pct = airplay_volume_to_pct,
  .write = airplay_write,
  .metadata_prepare = airplay_metadata_prepare,
  .metadata_send = airplay_metadata_send,
  .metadata_purge = airplay_metadata_purge,
  .device_authorize = airplay_device_authorize,
};
