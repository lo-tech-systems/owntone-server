# Artwork

In this fork, cover artwork is retrieved from the pipe metadata input: a picture
pushed alongside the current track over the metadata pipe is decoded and held in
memory for the session, then served straight from there to AirPlay/MRP receivers
and to the legacy DAAP artwork request.

See the pipe metadata section of
[AirPlay devices/speakers](audio-outputs/airplay.md#now-playing-on-apple-tv)
and the `/api/metadata` `artwork_url` parameter in the [JSON API](json-api.md#metadata)
for the two ways artwork reaches a queue item in this fork.
