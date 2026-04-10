# OwnTone-Minimal

OwnTone-Minimal is a stripped-down, headless build of [OwnTone](https://github.com/owntone/owntone-server)
designed to act as a lightweight AirPlay output engine for external audio pipelines.

It reads PCM audio from a named pipe (FIFO), streams it to one or more AirPlay 1 and/or AirPlay 2
speakers, and is controlled entirely via an HTTP JSON API.

---

## Features

* FIFO (named pipe) audio input — compatible with Shairport-Sync and similar tools
* AirPlay 1 (RAOP) and AirPlay 2 output to multiple simultaneous speakers
* Metadata forwarding — title/artist/album/artwork pushed to AirPlay speakers
* JSON config file (`owntone-settings.json`) — no SQLite database
* HTTP JSON API for all control and settings
* No web UI, no library scanning, no external service integrations

---

## Use Case

OwnTone-Minimal is intended as a backend audio output service within a larger pipeline.

```text
Audio source → DSP / processing → FIFO pipe → OwnTone-Minimal → AirPlay devices
                                                     ↑
                                              JSON API control
```

---

## Dependencies

| Library | Purpose |
|---------|---------|
| libevent (≥ 2.1.4) | Event loop, HTTP server |
| json-c | JSON config and API |
| zlib | Compression |
| libavcodec / libavformat / libavfilter | PCM decode, ALAC encode |
| libplist | AirPlay 2 pairing |
| libsodium | Cryptography |
| libgcrypt + libgpg-error | AirPlay 1 encryption |
| avahi / DNS-SD | mDNS speaker discovery |

Removed from upstream: SQLite, libconfuse, libcurl, libxml2, libunistring, inotify, ALSA.

---

## HTTP API

See [docs/json-api.md](docs/json-api.md) for full API documentation.

Key endpoints:

| Method | Endpoint | Description |
|--------|----------|-------------|
| PUT | `/api/player/play` | Start playback |
| PUT | `/api/player/stop` | Stop playback |
| GET | `/api/outputs` | List AirPlay speakers |
| PUT | `/api/outputs/{id}` | Enable/disable/configure a speaker |
| PUT | `/api/metadata` | Push track metadata to active outputs |
| PUT | `/api/update` | Reload config |
| GET/PUT | `/api/settings/{category}/{option}` | Read/write runtime settings |

---

## Platform

Linux only (uses POSIX pipes, Avahi mDNS, POSIX timers).

---

## Lineage and Fork Relationship

OwnTone-Minimal is a fork of [OwnTone](https://github.com/owntone/owntone-server), which was
previously forked-daapd, itself a rewrite of mt-daapd (Firefly Media Server).

The AirPlay 1 (`src/outputs/raop.c`) and AirPlay 2 (`src/outputs/airplay.c`) protocol
implementations are inherited unchanged from OwnTone. All protocol-level decisions belong to the
OwnTone project.

See [FORK.md](FORK.md) for a detailed description of what was changed and why.

---

## License

GNU General Public License v2.0, inherited from OwnTone. See [LICENSE](LICENSE).
