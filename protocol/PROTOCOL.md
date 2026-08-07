# PadBridge wire protocol v1

PadBridge transports independently decodable, low-latency media messages. All
integer fields are unsigned and big-endian. TCP is used for the first USB
checkpoint; the same message bodies can later ride over a datagram transport.

## Header (24 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `PDB1` |
| 4 | 1 | protocol version (`1`) |
| 5 | 1 | message type |
| 6 | 2 | flags |
| 8 | 4 | payload length |
| 12 | 4 | sequence number |
| 16 | 8 | monotonic timestamp in nanoseconds |

The maximum accepted payload is 16 MiB. A receiver must disconnect on invalid
magic, version, or oversized payload instead of attempting to resynchronize.

## Message types

| Value | Name | Direction | Payload |
|---:|---|---|---|
| 1 | hello | either | UTF-8 implementation string |
| 2 | video config | host to iPad | structure below |
| 3 | video frame | host to iPad | one H.264 Annex-B access unit |
| 4 | pointer | iPad to host | structure below |
| 5 | audio config | host to iPad | reserved for checkpoint 2 |
| 6 | audio frame | host to iPad | reserved for checkpoint 2 |
| 7 | ping | either | empty |
| 8 | pong | either | empty |
| 9 | stats | either | reserved |

Header flag bit 0 marks a keyframe and bit 1 marks a discontinuity.

## Video config (16 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | encoded width |
| 2 | 2 | encoded height |
| 4 | 2 | requested refresh rate |
| 6 | 1 | codec (`1` = H.264) |
| 7 | 1 | pixel format (`1` = NV12) |
| 8 | 4 | target bitrate in bits/second |
| 12 | 4 | reserved, zero |

The first keyframe after a config message must contain SPS and PPS NAL units.
Frames use Annex-B start codes; the iPad client converts them to AVCC before
submitting them to VideoToolbox.

## Pointer event (36 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | event id |
| 4 | 1 | phase (`0` down, `1` move, `2` up, `3` cancel) |
| 5 | 1 | tool (`0` finger, `1` Pencil) |
| 6 | 2 | button mask |
| 8 | 4 | normalized X, IEEE-754 float |
| 12 | 4 | normalized Y, IEEE-754 float |
| 16 | 4 | normalized pressure, IEEE-754 float |
| 20 | 4 | X tilt in radians, IEEE-754 float |
| 24 | 4 | Y tilt in radians, IEEE-754 float |
| 28 | 8 | event timestamp in nanoseconds |

