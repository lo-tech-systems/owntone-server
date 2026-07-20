# Fork Relationship: owntone-mini vs OwnTone

owntone-mini is a fork of [OwnTone](https://github.com/owntone/owntone-server).
This file documents what was removed, what was changed, and the rationale.

---

## Scope

owntone-mini has a single purpose: stream audio from a named pipe (FIFO) to one or more
AirPlay 1 and/or AirPlay 2 speakers, controlled by an HTTP JSON API.

Everything outside that scope has been removed.

---

## What was removed

### Features

| Removed feature | Rationale |
|----------------|-----------|
| Web interface | Not needed for headless API-only use |
| Library scanning and SQLite database | No file library; queue is a single in-memory pipe item |
| Spotify / LastFM / Chromecast / MPD integrations | Out of scope |
| Smart playlist parser (flex/bison) | No library |
| ALSA / PulseAudio output | Audio stays inside the pipeline; AirPlay is the only output |
| Artwork cache | Cache used SQLite; artwork is now loaded directly from `artwork_url` |
| HTTP push notifications (websockets) | No web clients |

### Dependencies

| Removed dependency | Reason removed |
|--------------------|---------------|
| SQLite3 | Queue and speaker state are now in-memory |
| libconfuse | Config is now a JSON file (`owntone-settings.json`) |
| libcurl | Was used for artwork download; removed with cache |
| libxml2 | Was used for smart playlist parsing |
| libunistring | Was used in `unicode_fixup_string`; no library scanning means no charset fixup |
| inotify | Was used to watch the library directory |
| libmount | Was used to detect mount events |
| flex / bison | Were used for the smart playlist parser |

### Source files deleted

```
src/db.c / src/db.h
src/db_init.c / src/db_init.h
src/db_upgrade.c / src/db_upgrade.h
src/cache.c / src/cache.h
src/library.c / src/library.h
src/conffile.c / src/conffile.h
src/settings.c / src/settings.h
src/parsers/smartpl_lexer.l
src/parsers/smartpl_parser.y
sqlext/sqlext.c / sqlext/Makefile.am
```

---

## What was changed

### New files

| File | Purpose |
|------|---------|
| `src/queue.c` / `src/queue.h` | In-memory single-item queue (replaces SQLite-backed `db_queue_*`) |
| `src/owntone_config.c` / `src/owntone_config.h` | JSON config reader (replaces libconfuse `conffile.c`) |
| `src/outputs/airplay_buffered.c` / `.h` | AirPlay 2 buffered-audio (stream type 103) transport and ChaCha20-Poly1305 framing |
| `src/outputs/airplay_encoder.c` / `.h` | Threaded per-transform audio encoder feeding the buffered transport |
| `src/outputs/airplay_common.h` | Shared definitions for the buffered/encoder units |

### Modified files (summary)

| File | Change |
|------|--------|
| `src/player.c` / `src/player.h` | Removed seek, shuffle, repeat, multi-queue, verification kickoff |
| `src/transcode.c` / `src/transcode.h` | Reduced to encode-only path (PCM → ALAC/PCM16); removed file decode, seeking, metadata extraction. Extended with AirPlay 2 encode profiles (48 kHz AAC stereo, AAC 5.1, 24-bit ALAC), ffmpeg surround-upmix filters, and CPU-class AAC coder selection |
| `src/outputs.c` | Removed XCODE_PCM24/32/UNKNOWN dead references |
| `src/misc.c` / `src/misc.h` | Removed: `unicode_fixup_string`, `two_str_hash`, `keyval_sort`, `linear_regression`, `m_readfile`, `atrim`; removed libunistring includes |
| `src/listener.h` | Reduced to 3 event types: PLAYER, VOLUME, SPEAKER |
| `src/logger.c` / `src/logger.h` | Removed unused log domains; removed `logger_alsa` |
| `src/outputs/raop.c` | Fixed `raop_metadata_prepare` to build DMAP text buffer and load file artwork |
| `src/outputs/airplay.c` | Substantially extended: AirPlay 2 buffered-audio output (RTP type 0x67 / stream type 103) with ChaCha20-Poly1305 framing; PTP-timed playback to HomePod stereo pairs (SETPEERS peer setup, timing-anchor handling); stream-type and audio-format selection/capability negotiation; 5.1 surround to a standalone Apple TV; connection retry/backoff; plus the `airplay_metadata_prepare` DMAP/artwork fix |
| `src/libairptp/` / `src/ptpd.*` | Inherited from OwnTone and tuned: PTP grandmaster/announce settings for prompt receiver lock (not owntone-mini-authored) |
| `src/httpd_jsonapi.c` | Added `PUT /api/metadata` endpoint |
| `configure.ac` | Removed: LIBCURL, LIBXML2, INOTIFY, libunistring, AM_ICONV, sqlext |
| `Makefile.am` | Removed `sqlext` from SUBDIRS |

---

## Upstream

https://github.com/owntone/owntone-server

OwnTone is licensed under the GNU General Public License v2.0.
owntone-mini inherits the same licence.
