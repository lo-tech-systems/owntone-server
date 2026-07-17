---
hide:
  - navigation
---

# API Endpoint Reference

This pared-back build exposes a small HTTP API for:

- starting and stopping FIFO-backed playback
- discovering and controlling AirPlay and AirPlay 2 outputs
- reading and updating a small set of runtime settings
- retrieving basic server status

All endpoints are rooted at `/api`.

## Player

| Method | Endpoint | Description |
| ------ | -------- | ----------- |
| PUT | `/api/player/play` | Start or resume playback |
| PUT | `/api/player/stop` | Stop playback |

### Start playback

**Endpoint**

```http
PUT /api/player/play
```

**Query parameters**

| Parameter | Type | Value |
| --------- | ---- | ----- |
| `item_id` | integer | Optional queue item id to start |
| `position` | integer | Optional queue position to start |

If no query parameter is provided, playback starts or resumes the current source.

**Response**

Returns HTTP `204 No Content` on success.

**Example**

```shell
curl -X PUT "http://localhost:3689/api/player/play"
```

### Stop playback

**Endpoint**

```http
PUT /api/player/stop
```

**Response**

Returns HTTP `204 No Content` on success.

**Example**

```shell
curl -X PUT "http://localhost:3689/api/player/stop"
```

## Metadata

Push track metadata (title, artist, album, artwork) to all active AirPlay outputs.
This is useful when the audio source does not embed metadata (e.g. raw PCM on a pipe).

| Method | Endpoint | Description |
|--------|----------|-------------|
| PUT | `/api/metadata` | Push track metadata to all active outputs |

### Example: push text metadata

```
PUT /api/metadata
```

**Body parameters**

| Parameter | Type | Description |
|-----------|------|-------------|
| title | string | Track title |
| artist | string | Artist name |
| album | string | Album name |
| artwork_url | string | Local artwork path as `file:/absolute/path.jpg` or `.png` |

All fields are optional; omitted fields are not changed.

**Example**

```shell
curl -X PUT "http://localhost:3689/api/metadata" \
     -H "Content-Type: application/json" \
     -d '{"title":"My Track","artist":"My Artist","album":"My Album","artwork_url":"file:/var/music/cover.jpg"}'
```

## Outputs

| Method | Endpoint | Description |
| ------ | -------- | ----------- |
| GET | `/api/outputs` | List all discovered outputs |
| GET | `/api/outputs/{id}` | Get a single output |
| PUT | `/api/outputs/{id}` | Update a single output |
| PUT | `/api/outputs/set` | Replace the selected output set |

### Output object

| Key | Type | Value |
| --- | ---- | ----- |
| `id` | string | Output id |
| `name` | string | Display name |
| `type` | string | Output type |
| `selected` | boolean | `true` if the output is enabled |
| `has_password` | boolean | `true` if the output advertises password protection |
| `requires_auth` | boolean | `true` if the output currently requires authorization |
| `needs_auth_key` | boolean | `true` if the server does not have a valid stored auth key |
| `volume` | integer | Output volume in percent |
| `offset_ms` | integer | Timing offset in milliseconds |
| `format` | string | Active audio format |
| `supported_formats` | array | Supported audio format strings |
| `mode` | string | Active protocol mode preference. One of `"auto"`, `"raop"`, `"airplay2"`, `"airplay2_buffered"`, `"airplay2_buffered_24"`, `"airplay2_surround_stereo"`, or `"airplay2_surround_upmix"` |
| `supported_modes` | array | Concrete protocol modes available for this output; a subset of `["raop", "airplay2", "airplay2_buffered", "airplay2_buffered_24", "airplay2_surround_stereo", "airplay2_surround_upmix"]`. Reflects capability learned from the device itself, so it can grow after the output is first discovered. Absent or empty means only one protocol is available |

The AirPlay 2 modes beyond plain `"airplay2"` (realtime) select a buffered
transport, where the receiver queues audio ahead of playout instead of
receiving it as it plays:

| Mode | Description |
| ---- | ----------- |
| `airplay2_buffered` | Buffered playback with 16-bit AAC-LC audio. Offered as a baseline for any AirPlay 2-capable output as soon as the output is discovered, since it is supported almost universally; the session falls back to realtime if the device turns out not to accept it |
| `airplay2_buffered_24` | Buffered playback with 24-bit ALAC (lossless). Only listed once learned from the device; only worth selecting when the source is genuinely higher than 16-bit |
| `airplay2_surround_stereo` | Buffered 5.1 output that maps a stereo source onto the front-left/front-right/LFE channels of a 5.1 stream, leaving the remaining channels silent. Only listed for a standalone Apple TV |
| `airplay2_surround_upmix` | Buffered 5.1 output that decodes and upmixes a stereo source across all six channels. Only listed for a standalone Apple TV |

`airplay2_surround_stereo` and `airplay2_surround_upmix` never appear in
`supported_modes` for a HomePod, a HomePod stereo pair, or a HomePod group
routed through an Apple TV — surround is offered only for a standalone Apple
TV.

### List outputs

**Endpoint**

```http
GET /api/outputs
```

**Response**

| Key | Type | Value |
| --- | ---- | ----- |
| `outputs` | array | Array of [output](#output-object) objects |

**Example**

```shell
curl -X GET "http://localhost:3689/api/outputs"
```

```json
{
  "outputs": [
    {
      "id": "1",
      "name": "Living Room",
      "type": "AirPlay 2",
      "selected": true,
      "has_password": false,
      "requires_auth": false,
      "needs_auth_key": false,
      "volume": 50,
      "offset_ms": 0,
      "format": "alac",
      "supported_formats": ["pcm", "alac"],
      "mode": "auto",
      "supported_modes": ["raop", "airplay2"]
    }
  ]
}
```

### Get an output

**Endpoint**

```http
GET /api/outputs/{id}
```

**Response**

Returns a single [output](#output-object) object.

### Update an output

**Endpoint**

```http
PUT /api/outputs/{id}
Content-Type: application/json
```

**Request body**

All fields are optional. Any combination may be supplied in one request.

| Key | Type | Value |
| --- | ---- | ----- |
| `selected` | boolean | Enable or disable the output |
| `volume` | integer | Set output volume |
| `pin` | string | Submit a PIN for authorization |
| `format` | string | Set the output audio format |
| `offset_ms` | integer | Set timing offset in milliseconds |
| `mode` | string | Set the protocol mode preference: `"auto"`, `"raop"`, `"airplay2"`, `"airplay2_buffered"`, `"airplay2_buffered_24"`, `"airplay2_surround_stereo"`, or `"airplay2_surround_upmix"` |

**Response**

Returns HTTP `204 No Content` on success.

**Example**

```shell
curl -X PUT "http://localhost:3689/api/outputs/1" \
  -H "Content-Type: application/json" \
  -d "{\"selected\":true,\"volume\":60}"
```

### Replace selected outputs

**Endpoint**

```http
PUT /api/outputs/set
Content-Type: application/json
```

**Request body**

| Key | Type | Value |
| --- | ---- | ----- |
| `outputs` | array | Array of output id strings to select |

**Response**

Returns HTTP `204 No Content` on success.

**Example**

```shell
curl -X PUT "http://localhost:3689/api/outputs/set" \
  -H "Content-Type: application/json" \
  -d "{\"outputs\":[\"1\",\"2\"]}"
```

## Server Info

| Method | Endpoint | Description |
| ------ | -------- | ----------- |
| GET | `/api/config` | Get configuration information |
| GET | `/api/library` | Get basic health/status information |
| PUT | `/api/update` | Reload configuration, apply any changed pipe settings live, and refresh the in-memory pipe item |

### Config

**Endpoint**

```http
GET /api/config
```

**Response**

| Key | Type | Value |
| --- | ---- | ----- |
| `product_name` | string | Product/package name compiled into the server |
| `version` | string | Server version |
| `websocket_port` | integer | Always `0` in this build |
| `buildoptions` | array | Array of server feature strings |
| `restart_required` | boolean | `true` if a restart-required config change is pending |

**Example**

```shell
curl -X GET "http://localhost:3689/api/config"
```

```json
{
  "product_name": "owntone-mini",
  "websocket_port": 0,
  "version": "29.0-mini.6",
  "buildoptions": [
    "ffmpeg",
    "Without Spotify",
    "Without LastFM",
    "Without Chromecast",
    "Without MPD",
    "Without websockets",
    "Without ALSA",
    "Without webinterface",
    "Regex"
  ],
  "restart_required": false
}
```

### Library

This endpoint is retained as a lightweight health/status response.

**Endpoint**

```http
GET /api/library
```

**Response**

| Key | Type | Value |
| --- | ---- | ----- |
| `updating` | boolean | Always `false` in this build |
| `songs` | integer | Always `1` in this build |
| `Healthy` | boolean | Always `true` in this build |
| `restart_required` | boolean | `true` if a restart-required config change is pending |

**Example**

```json
{
  "updating": false,
  "songs": 1,
  "Healthy": true,
  "restart_required": false
}
```

### Update

Reloads configuration, applies any changed pipe settings live, and refreshes the in-memory synthetic pipe queue item.

If `misc/pipe_path` or `misc/pipe_autostart` changed on disk, the running pipe input is reconfigured immediately.
If there is no relevant change, this endpoint is effectively a no-op for the pipe runtime.

**Endpoint**

```http
PUT /api/update
```

**Response**

Returns HTTP `204 No Content` on success.

Returns HTTP `500 Internal Server Error` with a JSON error object if the updated pipe configuration cannot be applied.

## Settings

| Method | Endpoint | Description |
| ------ | -------- | ----------- |
| GET | `/api/settings/{category}/{option}` | Read a setting |
| PUT | `/api/settings/{category}/{option}` | Change a setting |

There are no category-listing or option-listing endpoints in this build.

### Option object

| Key | Type | Value |
| --- | ---- | ----- |
| `name` | string | Option name |
| `type` | integer | `0` = integer, `1` = boolean, `2` = string |
| `value` | varies | Current value in use. For `misc/pipe_path` this is the live path currently used by the running server, which may differ from the persisted on-disk value until `PUT /api/update` applies the change. |

### Supported settings

| Endpoint | Type | Notes |
| -------- | ---- | ----- |
| `/api/settings/misc/loglevel` | integer | Log level |
| `/api/settings/misc/pipe_path` | string | Pipe/FIFO path. The provided path must already exist, be a FIFO, and be readable by the server. A successful PUT persists the value immediately; `PUT /api/update` makes it live. GET returns the live path currently in use. |
| `/api/settings/misc/pipe_autostart` | boolean | Whether the pipe input autostarts. Changes become live on `PUT /api/update`. |
| `/api/settings/misc/ipv6` | boolean | IPv6 enable/disable, restart required |
| `/api/settings/player/start_buffer_ms` | integer | Start buffer in milliseconds, valid range `300` to `3500`, restart required |
| `/api/settings/player/uncompressed_alac` | boolean | ALAC output preference, restart required |
| `/api/settings/player/buffered_audio_enabled` | boolean | Prefer buffered AAC-LC over realtime for AirPlay 2 outputs left on `auto`; takes effect on the next output start |

### Get a setting

**Endpoint**

```http
GET /api/settings/{category}/{option}
```

**Example**

```shell
curl -X GET "http://localhost:3689/api/settings/player/start_buffer_ms"
```

```json
{
  "name": "start_buffer_ms",
  "type": 0,
  "value": 2250
}
```

For `misc/pipe_path`, the response reflects the current live pipe path in use by the running process. If a new value has been written but `PUT /api/update` has not yet been called, GET continues to return the old live value.

### Change a setting

**Endpoint**

```http
PUT /api/settings/{category}/{option}
Content-Type: application/json
```

**Request body**

```json
{
  "value": 1500
}
```

**Response**

On success returns HTTP `200 OK` with:

```json
{
  "restart_required": true
}
```

`restart_required` reflects whether the server currently has any pending restart-required configuration changes.

For settings that can be applied live, such as `misc/pipe_path`, this field may still be `false` if there are no other pending restart-required changes.

For `misc/pipe_path`, the server validates the provided path before persisting it. The path must exist, be a FIFO, and be accessible to the server process.

`PUT /api/settings/misc/pipe_path` does not switch the running pipe immediately. The new value is persisted, and `PUT /api/update` applies it to the live runtime.

If validation fails, the endpoint returns HTTP `400 Bad Request` with a JSON body like:

```json
{
  "error": "Invalid pipe_path: '/tmp/example.fifo' does not exist, is not a FIFO, or is not accessible"
}
```

**Examples**

```shell
curl -X PUT "http://localhost:3689/api/settings/misc/ipv6" \
  -H "Content-Type: application/json" \
  -d "{\"value\":false}"
```

```shell
curl -X PUT "http://localhost:3689/api/settings/player/start_buffer_ms" \
  -H "Content-Type: application/json" \
  -d "{\"value\":1500}"
```

```shell
curl -X PUT "http://localhost:3689/api/settings/misc/pipe_path" \
  -H "Content-Type: application/json" \
  -d "{\"value\":\"/tmp/autostream-pipes/autostream.fifo\"}"
```
