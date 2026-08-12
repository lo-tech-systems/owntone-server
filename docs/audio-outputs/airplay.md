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
or offered automatically under `auto` while `buffered_audio_enabled` is on
(the default) in the [configuration](../configuration.md#general-buffered_audio_enabled).

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

## Now Playing on Apple TV

When streaming to an Apple TV, OwnTone registers as a MediaRemote now-playing
client and pushes track metadata (title, artist, album, artwork, position) over
the AirPlay control channel. This drives the Apple TV's on-screen Now Playing
UI, and tvOS then treats the stream as active music playback — in particular,
the "don't show screensaver when playing music" setting works as expected.
Legacy AirPlay metadata alone is accepted but ignored by current tvOS versions,
which is why this dedicated path exists.

This is automatic for Apple TV receivers that advertise support (AirPlay
feature bit 50), and does not run for any other receiver type. To turn it off
for a specific device, set `nowplaying_disable = true` in that device's
`airplay` section in the configuration file.

Note that the Apple TV typically shows a small source indicator immediately,
and brings up the full Now Playing screen a little later — that delay is the
receiver's own UI behaviour.

Artwork sent to the Apple TV over this path comes from the pipe metadata
input's in-memory picture: a picture pushed alongside the current track over
the metadata pipe is decoded once and held in RAM for the life of the
session (never written to disk), and served from there for every push
described below.

Each NowPlayingInfo push is followed by a one-shot re-assert of the same
payload roughly 5 seconds later, and a slower re-assert is also piggybacked
on the periodic keep-alive request. Both exist because a receiver has been
observed to occasionally accept a valid, size-compliant NowPlayingInfo push
(including its artwork) without ever rendering it — no error is reported and
no re-request follows, so the sender has no way to detect the drop other than
re-asserting. Since only one push happens per track change, a single silent
drop would otherwise leave that track's Now Playing screen blank (or stale)
for its whole duration; the two re-asserts bound how long that can last.

The MediaRemote message exchange implemented here was worked out by studying
the on-wire behaviour of the cliairplay sender from the
[airplay-cli](https://github.com/music-assistant/airplay-cli) project, whose
authors did the hard work of mapping what a receiver actually requires. The
code in OwnTone is an independent implementation.

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
