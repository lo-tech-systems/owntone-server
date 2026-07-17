# OwnTone-Minimal

OwnTone-Minimal is a stripped-down, headless build of [OwnTone](https://github.com/owntone/owntone-server)
designed to act as a lightweight AirPlay output engine for external audio pipelines.

It reads PCM audio from a named pipe (FIFO), streams it to one or more AirPlay 1 and/or AirPlay 2
speakers, and is controlled entirely via an HTTP JSON API.

---

## Features

* FIFO (named pipe) audio input — compatible with Shairport-Sync and similar tools
* Playback via:
** AirPlay 1 (RAOP) and AirPlay 2 compatible speakers
** HomePods, including stereo pairs
** Playback via Apple TV, either through HDMI or connected HomePods
* AirPlay 2 buffered output (16-bit AAC-LC or 24-bit lossless ALAC), with
  optional 5.1 surround for a standalone Apple TV
* HTTP JSON API for all control and settings

This build has no web UI, no library scanning, and no external service integrations.

---

## Use Case

OwnTone-Minimal is intended as a backend audio output service within a larger pipeline.
Supported output devices include AirPlay 1 speakers, AirPlay 2 speakers, HomePods
(including stereo pairs), and Apple TV (routing audio to its connected HomePods).

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

## Protocol Compatibility and Intellectual Property Notice

OwnTone-Minimal is an independent interoperability project. Its AirPlay 1 and AirPlay 2 playback
support — including HomePod, HomePod stereo pairs, and Apple TV proxy routing — is provided only
so independently created software can exchange audio with compatible devices on a local network.

This project does not include Apple source code, Apple binaries, Apple firmware, Apple artwork, or
Apple confidential documentation. Contributors must not submit code, test material, keys,
certificates, protocol dumps, or documentation obtained under an Apple NDA or from any non-public
Apple source.

The terms "Apple", "AirPlay", "AirPlay 2", "Apple TV", "HomePod", and related names are used only
to identify compatible devices and protocols. OwnTone-Minimal is not affiliated with, endorsed by,
sponsored by, or certified by Apple Inc.

Reverse-engineered protocol behavior should be documented and implemented only to the extent
necessary for interoperability with independently created software and lawful user-owned devices.
Any copyright, trademark, or other intellectual-property concern should be reported to the
maintainers so it can be reviewed promptly.

---

## License

GNU General Public License v2.0, inherited from OwnTone. See [LICENSE](LICENSE).
