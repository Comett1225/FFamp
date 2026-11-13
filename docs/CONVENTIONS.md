# Conventions shared by every reader, decoder and writer

Every reader fills `absent::Track` the same way, so any decoder or writer works with any container.

## Track.codec (fourcc) and Track.config

| Stream | codec | configType | config bytes |
|---|---|---|---|
| H.264 | `avc1` | `avcC` | AVCDecoderConfigurationRecord (samples are length-prefixed NAL units, 4-byte lengths unless avcC says otherwise) |
| HEVC | `hvc1` (also for `hev1` input) | `hvcC` | HEVCDecoderConfigurationRecord, samples length-prefixed |
| AV1 | `av01` | `av1C` | AV1CodecConfigurationRecord; samples are low-overhead OBU temporal units |
| VP9 | `vp09` | `vpcC` (may be empty) | VPCodecConfigurationRecord body (version/flags + fields), or empty |
| VP8 | `vp08` | `vpcC` (may be empty) | as VP9 |
| ProRes | the ProRes fourcc as found (`apch`, `apcn`, `apcs`, `apco`, `ap4h`, `ap4x`) | 0 | empty |
| MPEG-4 part 2 | `mp4v` | `esds` | esds body |
| AAC (any profile) | `mp4a` | `esds` (from MP4) or `ASC ` (raw AudioSpecificConfig, from Matroska, ADTS, TS) | as named |
| MP3 / MP2 | `.mp3` / `.mp2` | 0 | empty |
| Opus | `Opus` | `OpHd` | OpusHead exactly as in Ogg (starts with "OpusHead", little-endian fields); MP4 dOps is converted to this |
| FLAC | `fLaC` | `fLaC` | the 34-byte STREAMINFO block body |
| ALAC | `alac` | `alac` | the 24-byte ALACSpecificConfig (magic cookie) |
| Vorbis | `vorb` | `vorb` | the three header packets in Xiph lacing (as Matroska CodecPrivate) |
| AC-3 / E-AC-3 | `ac-3` / `ec-3` | 0 | empty (no decoder; probe only) |
| PCM | `lpcm` | 0 | empty; `bits` (8, 16, 24, 32, 64), `floating`, `bigEndian` describe it; 8-bit is unsigned, others signed |

## Timing

- `timescale`: ticks per second for `Sample.pts/dts/duration`. Audio tracks should use the sample rate when the container allows it.
- `mediaStart`: first presented tick (edit list start, Opus pre-skip, MP3 encoder delay + 529, Matroska CodecDelay). Samples before it are
  decoded but not shown.
- `mediaEnd`: tick where presentation ends (-1 = until the last sample ends). Used for encoder padding (LAME tag, Ogg end granule,
  edit list duration).
- `order`: indexes of shown samples sorted by pts; `syncs`: key frames in decode order.
- Matroska and TS have no stored dts: readers synthesise a non-decreasing dts not above pts.

## Reading packets

`Reader::read(track, index, out)` writes exactly `samples[index].size` bytes: the codec packet as the decoder wants it (no ADTS header,
no PES header, Annex-B already turned into length-prefixed NAL units). Readers whose packets are split in the file keep their own piece
list; `Sample.offset` is then free for the reader's use.

## Rules

- C++20. No comments in any source file or script. Clear names instead.
- Every count and size read from a file is checked against what is left before it is used; a broken file gives an error, never a crash.
- Optional libraries are guarded with their ABSENT_* define (see CMakeLists.txt).
- Commits as `AbsentMotion <noreply@absentmotion.invalid>`, no co-author lines.
