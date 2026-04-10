# Fork Relationship: OwnTone-Minimal vs OwnTone

OwnTone-Minimal is a fork of [OwnTone](https://github.com/owntone/owntone-server).
This file documents what was removed, what was changed, and the rationale.

---

## Scope

OwnTone-Minimal has a single purpose: stream audio from a named pipe (FIFO) to one or more
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

### Modified files (summary)

| File | Change |
|------|--------|
| `src/player.c` / `src/player.h` | Removed seek, shuffle, repeat, multi-queue, verification kickoff |
| `src/transcode.c` / `src/transcode.h` | Reduced to encode-only path (PCM → ALAC/PCM16); removed file decode, seeking, metadata extraction |
| `src/outputs.c` | Removed XCODE_PCM24/32/UNKNOWN dead references |
| `src/misc.c` / `src/misc.h` | Removed: `unicode_fixup_string`, `two_str_hash`, `keyval_sort`, `linear_regression`, `m_readfile`, `atrim`; removed libunistring includes |
| `src/listener.h` | Reduced to 3 event types: PLAYER, VOLUME, SPEAKER |
| `src/logger.c` / `src/logger.h` | Removed unused log domains; removed `logger_alsa` |
| `src/outputs/raop.c` | Fixed `raop_metadata_prepare` to build DMAP text buffer and load file artwork |
| `src/outputs/airplay.c` | Fixed `airplay_metadata_prepare` to build DMAP text buffer and load file artwork |
| `src/httpd_jsonapi.c` | Added `PUT /api/metadata` endpoint |
| `configure.ac` | Removed: LIBCURL, LIBXML2, INOTIFY, libunistring, AM_ICONV, sqlext |
| `Makefile.am` | Removed `sqlext` from SUBDIRS |

---

## What was NOT changed

The AirPlay protocol implementation files are inherited verbatim from OwnTone:

* `src/outputs/raop.c` — AirPlay 1 (RAOP) — **only metadata_prepare was modified**
* `src/outputs/airplay.c` — AirPlay 2 — **only metadata_prepare was modified**

All protocol-level decisions in these files belong to the OwnTone project. Any questions about
the AirPlay implementation should be directed there.

---

## Upstream

https://github.com/owntone/owntone-server

OwnTone is licensed under the GNU General Public License v2.0.
OwnTone-Minimal inherits the same licence.
