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
      "supported_formats": ["pcm", "alac"]
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
| PUT | `/api/update` | Reload configuration and refresh the in-memory pipe item |

### Config

**Endpoint**

```http
GET /api/config
```

**Response**

| Key | Type | Value |
| --- | ---- | ----- |
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
  "websocket_port": 0,
  "version": "0.0.0",
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

Reloads configuration and refreshes the in-memory synthetic pipe queue item.

**Endpoint**

```http
PUT /api/update
```

**Response**

Returns HTTP `204 No Content` on success.

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
| `value` | varies | Current value in use. For `misc/pipe_path` this is the live path currently used by the running server, which may differ from the pending on-disk value after a restart-required PUT. |

### Supported settings

| Endpoint | Type | Notes |
| -------- | ---- | ----- |
| `/api/settings/misc/loglevel` | integer | Log level |
| `/api/settings/misc/pipe_path` | string | Pipe/FIFO path, persisted immediately but not applied until restart. GET returns the live path currently in use. |
| `/api/settings/misc/pipe_autostart` | boolean | Whether the pipe input autostarts |
| `/api/settings/misc/ipv6` | boolean | IPv6 enable/disable, restart required |
| `/api/settings/player/start_buffer_ms` | integer | Start buffer in milliseconds, valid range `300` to `3500`, restart required |
| `/api/settings/player/uncompressed_alac` | boolean | ALAC output preference, restart required |

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

For `misc/pipe_path`, the response reflects the current live pipe path in use by the running process. If a new value has been written but the service has not yet been restarted, GET continues to return the old live value.

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

`PUT /api/settings/misc/pipe_path` is restart-only in this build. The JSON settings file is updated immediately, but the running pipe input is not reloaded or switched over until the service restarts.

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
