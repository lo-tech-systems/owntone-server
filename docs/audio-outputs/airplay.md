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

## Permissions

If the speaker becomes unselected when you start playback, the log may show "ANNOUNCE
request failed in session startup: 400 Bad Request". In this case, try the Apple Home
app > Allow Speakers & TV Access > Anyone On the Same Network (or Everyone).
