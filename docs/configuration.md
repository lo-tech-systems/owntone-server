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

## Other settings

See the repository `owntone-settings.json` file for the current reference
defaults used by this fork.
