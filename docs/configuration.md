# Configuration

In this fork, the configuration is stored as JSON in `/etc/owntone-settings.json`
by default.

If the file is missing, the daemon will create it with safe defaults on startup.
The repository copy of `owntone-settings.json` is kept as a reference template.

## Format

Each setting consists of a name and a value. There are different types of settings: string, integer, boolean, and list.

Comments are preceded by a hash sign.

The format is as follow:

```conf
# Section
section {
    # String value
    setting = "<string-value>"
    # Integer value
    setting = <integer-value>
    # Boolean
    setting = <true|false>
    # List
    setting = { "value a", "value b", "value n"}
}
```

Some settings are device specific, in which case you add a section where you specify the device name in the heading. Say you're tired of loud death metal coming from your teenager's room:

```conf
airplay "Jared's Room" {
    max_volume = 3
}
```

## Most important settings

### general: uid

Identifier of the user running OwnTone.

Make sure that this user has read access to your configuration of `directories` in the `library` config section, and has write access to the database (`db_path`), cache directory (`cache_dir`) and log file (`logfile`). If you plan on using local audio then the user must also have access to that.

### library: directories

Path to the directory or directories containing the media to index (your library).

### general: buffered_audio_enabled

Boolean, default `true`. Also settable at runtime via
`PUT /api/settings/player/buffered_audio_enabled` (takes effect on the next
output start, no restart required).

When `true`, an AirPlay 2 output left on the `auto` mode preference prefers
buffered AAC-LC playback (`airplay2_buffered`) over realtime, but only on a
device that has advertised an actual AAC bufferStream format (learned from
GET /info) - not merely the mDNS "SupportsBufferedAudio" flag. A device that
has not advertised a concrete buffered format falls to `raop` (AirPlay 1), and
realtime `airplay2` is used only as a last resort for an AirPlay-2-only
receiver. When `false`, `auto` never uses buffered (`raop`, then realtime
`airplay2`).

The default is `true` so that outputs left on `auto` benefit from buffered
timing without manual configuration. Because the buffered path is still gated
per device on an advertised format, enabling it globally does not force
buffered onto a receiver that cannot play it.

This setting only affects outputs left on `auto`. An output with an explicit
`mode` selection (via the [output API](json-api.md#update-an-output)), such as
`airplay2_buffered_24` or one of the surround modes, always uses that mode
regardless of this setting.

### player: buffered_encoder_budget

Integer, default `0`. Also settable at runtime via
`PUT /api/settings/player/buffered_encoder_budget` (takes effect on the next
output activation, no restart required). Range 0-64.

Expert capacity-tuning knob for buffered AirPlay 2 outputs. Each active
buffered output (AAC-LC, ALAC 48k/24, or one of the 5.1 surround modes) costs
CPU on a dedicated encode thread; a new buffered output is refused rather than
started if it would push total cost over budget, so that already-playing
outputs are never starved.

`0` (auto) scores the host's core count and clock speed at startup and
derives a budget from that. Set a positive value to override the computed
budget - useful if the auto-detected value is too conservative or too
generous for your hardware, or to deliberately cap concurrent buffered
outputs. A rejected activation returns HTTP 503 with
`{"error": "encoder_capacity"}` (see [json-api.md](json-api.md)).

### player: resample_quality

String, `high` or `standard`, default `standard`. Also settable at runtime via
`PUT /api/settings/player/resample_quality` (takes effect on the next
playback session, no restart required).

Controls the resampler used for any output whose format doesn't match the
pipe input (for example an AirPlay 1 receiver at 44.1kHz/16-bit fed from a
48kHz/32-bit pipe). `standard` leaves libavfilter's auto-inserted resample
stage at its default settings. `high` requests the soxr resampler at VHQ
precision with dither, at a modest CPU cost. Outputs whose format already
matches the pipe (for example a 48kHz buffered AirPlay 2 receiver) never
insert a resample stage and are unaffected either way. An unrecognised value
is treated as `standard`.

### misc: user_agent

String, default derived from the running build (`<product>/<version>`). Also
settable at runtime via `PUT /api/settings/misc/user_agent` (restart
required).

Controls the `User-Agent` header sent on AirPlay RTSP requests. Useful when
diagnosing a receiver that treats this server differently from a real Apple
sender, since some receivers gate behaviour on the sender's advertised
version. Max 255 bytes, no control characters. Setting it to an empty string
removes the override and restores the derived default; GET always reports the
effective value, so it never appears blank.

## Other settings

See the repository `owntone-settings.json` file for the current reference
defaults used by this fork.
