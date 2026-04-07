# OwnTone-Minimal

OwnTone-Minimal is a stripped-down, headless build of OwnTone designed to act as a lightweight AirPlay output engine for external audio pipelines.

It provides:

* FIFO (pipe) audio input
* AirPlay 1 and AirPlay 2 output
* API-only control (no web UI)

This makes it suitable for automated or embedded use cases where audio is generated or processed by another service.

---

## Overview

OwnTone-Minimal removes non-essential functionality from OwnTone, including:

* Web interface
* Library/database management
* External service integrations (e.g. Spotify)

and retains only the components required for:

* receiving PCM audio via FIFO
* streaming to AirPlay receivers
* control via HTTP API

---

## Use Case

OwnTone-Minimal is intended to be used as a backend audio output service.

Typical pipeline:

```text
Audio source → DSP / processing → FIFO → OwnTone-Minimal → AirPlay devices
```

It is commonly used in conjunction with external controllers or audio processing services.

---

## Platform

* Linux only
* Requires ALSA-compatible system

---

## Lineage

OwnTone-Minimal is a fork of OwnTone, which was previously forked-daapd, itself a rewrite of mt-daapd (Firefly Media Server).

---

## License

This project is licensed under the GNU General Public License v2.0, as inherited from OwnTone.

See the LICENSE file for details.

---

## Notes

This is a specialized fork intended for headless and programmatic use.
For a full-featured media server, please refer to the upstream OwnTone project.

---
