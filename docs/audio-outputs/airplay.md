# AirPlay devices/speakers

OwnTone will discover AirPlay and AirPlay 2 devices available on your network using
mDNS.

## Authentication

Some devices require PIN verification, such as Apple TV.

PIN-based authorization is supported through the HTTP API. Authentication state for
each discovered output is also exposed through the API; see the API manual for details.

-- TODO - implement API password protection support
For devices that are password-protected, the device's AirPlay name and password
must be given in the configuration file. See the sample configuration file
for the syntax.

## Buffered output

AirPlay 2 outputs support an alternative to the default realtime stream: a
buffered transport where the receiver holds a queue of audio ahead of
playout instead of receiving it exactly as it plays. This is selected per
output via the `mode` field of the [output API](../json-api.md#update-an-output),
or offered automatically under `auto` when `buffered_audio_enabled` is turned
on in the [configuration](../configuration.md#general-buffered_audio_enabled).

Four buffered modes are available:

| Mode | Format | Use when |
| ---- | ------ | -------- |
| `airplay2_buffered` | 16-bit AAC-LC, stereo | General-purpose buffered playback; supported by essentially any AirPlay 2 receiver |
| `airplay2_buffered_24` | 24-bit ALAC, stereo (lossless) | The source audio is genuinely higher than 16-bit. Selecting it on a 16-bit source works but gains nothing — it just carries 16 real bits in a 24-bit container |
| `airplay2_surround_stereo` | 5.1, stereo mapped to front-left/front-right/LFE | A stereo source that should play through a full 5.1 speaker layout without a real surround mix, with the remaining channels left silent |
| `airplay2_surround_upmix` | 5.1, decoded/upmixed to all six channels | A stereo source that should be spread across all 5.1 channels |

The two surround modes are only offered and only accepted for a **standalone
Apple TV**. They are not available for a HomePod, a HomePod stereo pair, or a
HomePod group routed through an Apple TV (surround is hidden from
`supported_modes` for those cases, and selecting it has no effect).

A HomePod stereo pair is not driven as a single combined session: each
member of the pair is played to individually (one session per speaker), with
the pair's two speakers kept in phase via their own clock sync.

## Silent Speakers

There are at least two potential causes of apparently successful (but still silent)
playback:

1. Some speakers announce IPv6 support, but don't work with OwnTone. Disabling IPv6
   (requires restart) will resolve this.

2. If the overall offset is too short, the speaker may continually request
   retransmission and never actually play anything. The default values are 0 for
   the per-device offset_ms and 2,250ms for the start_buffer_ms. The combined value
   of these can generally be reduced (which reduces latency) to somewhere between
   500 to 1,000ms.

## Disappearing Speakers

If all previously discovered AirPlay outputs disappear after the server changes
network interface, OwnTone will attempt one in-process mDNS rediscovery after the
output list has remained empty for a short period.

## Permissions

If the speaker becomes unselected when you start playback, the log may show "ANNOUNCE
request failed in session startup: 400 Bad Request". In this case, try the Apple Home
app > Allow Speakers & TV Access > Anyone On the Same Network (or Everyone).
