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

Boolean, default `false`. Config-file setting only (no live-reload endpoint).

When `true`, an AirPlay 2 output left on the `auto` mode preference prefers
buffered AAC-LC playback (`airplay2_buffered`) over realtime on any device
that has advertised the buffered capability, before falling back to realtime
`airplay2` and then `raop`. When `false`, `auto` behaves as before (`airplay2`
realtime, then `raop`).

This setting only affects outputs left on `auto`. An output with an explicit
`mode` selection (via the [output API](json-api.md#update-an-output)), such as
`airplay2_buffered_24` or one of the surround modes, always uses that mode
regardless of this setting.

## Other settings

See the repository `owntone-settings.json` file for the current reference
defaults used by this fork.
