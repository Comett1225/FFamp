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

## Writers

- `Writer::open` picks the file type by extension: `.mp4`/`.m4v` (ISO MP4), `.mov` (QuickTime), `.m4a` (sound only), `.mkv`/`.mka`,
  `.webm` (VP8, VP9, AV1, Opus, Vorbis only), `.ogg`/`.opus`/`.oga` (one Opus track). A track the file type cannot hold makes
  `finish` fail with a message and no file is left behind.
- `TrackSpec` follows the table above. PCM (`lpcm`) also needs `bits`, `floating`, `bigEndian`; FLAC may give `bits`. `configType`
  for AAC may be `esds` or `ASC `; VP8/VP9 may have an empty config (MP4 gets a `vpcC` built from the first key frame).
- `skip` and `length` (track timescale) describe encoder priming and the exact shown length. `Writer::trim(track, skip, length)` sets
  them late, when the length is only known at the end.
- PCM packets are whole frames; MP4/MOV store them as one sample per frame, Matroska as one block per packet.
- Sound-only MP4/MOV/M4A files use the sample rate as the movie timescale so lengths are exact to the sample.
- `AudioFileWriter` rounds float samples the way FFmpeg does on arm64: truncate `x * 2^31` to 32 bits, then 16-bit =
  `(v + 0x8000) >> 16` (clamped), 24-bit = `v >> 8`.

## Presentation delay and file clocks (`Track.delay`)

- `delay` (track timescale) is the time on the file's own clock at which the tick `mediaStart` is shown. A sample is shown at
  `pts - mediaStart + delay`. Tracks of one file are kept in sync by their difference in `delay`; a consumer that wants the file to start
  at zero subtracts the smallest `delay` of all tracks (what the app does with ffprobe's format `start_time`).
- MP4/MOV: leading empty edits (media_time -1) add up to `delay`. The first media edit sets `mediaStart` and, when it has a duration,
  `mediaEnd`; edits that continue exactly where the previous one ended are joined to it. Later, separate segments are not played.
  Samples at or after `mediaEnd` are left out of `order`.
- MPEG-TS: timescale 90000 for every track. Each track's `dts` starts at 0; `mediaStart` is the pts of its first sample (the first key
  frame for video) and `delay` is that sample's time on the stream clock (33-bit wrap-around undone). A stream that starts less than a
  minute before the wrap gets negative times before it, as ffprobe shows it. `Track.id` is the PID.
- MPEG-TS video keeps parameter sets and SEI inside the samples as well as in `config`; access unit delimiters and filler data are
  dropped. H.264 samples are marked `sync` on IDR pictures only, HEVC on every IRAP picture. Two fields of one H.264 frame are one sample.
- MPEG audio layer I uses the codec `.mp1` (layer II `.mp2`, layer III `.mp3`).
- `Reader::format()` of the transport stream reader is `mpegts`.
