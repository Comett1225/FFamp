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
| APV | `apv1` | `apvC` | the `apvC` box body: 4 zero bytes, then the APVDecoderConfigurationRecord. Each sample is one access unit with its 4-byte size in front, then the `aPv1` signature (what FFmpeg stores in MP4) |
| ProRes | the ProRes fourcc as found (`apch`, `apcn`, `apcs`, `apco`, `ap4h`, `ap4x`) | 0 | empty |
| MPEG-4 part 2 | `mp4v` | `esds` | esds body (Matroska and AVI readers build one around the stream's own headers) |
| AAC (any profile) | `mp4a` | `esds` (from MP4) or `ASC ` (raw AudioSpecificConfig, from Matroska, ADTS, TS) | as named |
| MP3 / MP2 | `.mp3` / `.mp2` | 0 | empty |
| Opus | `Opus` | `OpHd` | OpusHead exactly as in Ogg (starts with "OpusHead", little-endian fields); MP4 dOps is converted to this |
| FLAC | `fLaC` | `fLaC` | the 34-byte STREAMINFO block body |
| ALAC | `alac` | `alac` | the 24-byte ALACSpecificConfig (magic cookie) |
| Vorbis | `vorb` | `vorb` | the three header packets in Xiph lacing (as Matroska CodecPrivate); decoded by libvorbis, the first packet of a run gives no sound |
| AC-3 / E-AC-3 | `ac-3` / `ec-3` | 0 | empty; a dependent E-AC-3 frame is part of the packet of the frame it extends |
| AMR-NB / AMR-WB | `samr` / `sawb` | 0 | empty; each frame starts with its one-byte frame type header, as in an .amr file |
| MPEG-1 / MPEG-2 video | `mp1v` / `mp2v` | 0 | empty; sequence headers stay inside the samples |
| Motion JPEG | `jpeg` | 0 | empty; one whole JPEG picture per sample |
| DV | `dvc ` (NTSC), `dvcp` (PAL), `dv5n`, `dv5p`, or the QuickTime fourcc as found | 0 | empty |
| H.263 | `h263` | 0 | empty |
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

## File tools (`include/absent/tools.h`, details in `docs/TOOLS.md`)

- `TrackSpec::delay` (track time scale) is the time before a track starts in the output. `Mp4Writer` writes a leading empty edit,
  `MkvWriter` adds it to every block time, the Ogg writer ignores it.
- `VideoDecoder::pts(index)` gives the time stamp of shown frame `index` without decoding.
- Times given to the tools count from the file's zero: the earliest shown sample of any video or sound track, with `Track.delay`
  taken into account.
- `Progress` is `bool(double fraction)`; returning `false` stops the tool, which then returns `false` with the error `cancelled` and
  leaves no file. Commands print `progress 0.42` lines and exit with 130 when stopped by SIGINT or SIGTERM.
- Tools write through the writers' `<output>.part` files; anything else they create (`<output>.spill.mp4`) is removed on every path.
- A stream copy that starts inside a sound track takes run-in packets before the cut (AAC 2, Opus and MP3 4, Vorbis 1) and hides
  them with `skip`.
- A source frame without colour labels is taken as BT.709 and not converted; labelled frames are converted to 8-bit BT.709 limited
  range before encoding (`Scaler` in `src/mac/tools_scale.cpp`).

## Decoded sound (`AudioDecoder`)

- Channel order by count, the same for every codec and for the mixer and the WAV writer: 3 = FL FR FC, 4 = FL FR FC BC (WAV mask 0x107),
  5 = FL FR FC BL BR, 6 = FL FR FC LFE BL BR, 7 = FL FR FC LFE BC SL SR (WAV mask 0x70F), 8 = FL FR FC LFE BL BR SL SR.
  FLAC comes out in its own file order, which is this order (4-channel FLAC and Opus are really quad: all four channels are kept).
- A gap between packets that the reader keeps, a packet that cannot be read and a packet the decoder rejects are played as silence of
  the same length, so the sound after them stays at its own time.
- `Writer::trim(track, skip, 0)` means nothing is shown: an encoder that was given no sound writes a file that decodes to 0 frames.
- `Mp4Writer` writes version 1 (64-bit) `mvhd`, `tkhd`, `mdhd` and `elst` boxes when a length does not fit in 32 bits.

## AVI, CAF, AC-3, AMR and older video (formats round)

- `Reader::format()` is `avi`, `caf`, `amr`, `ac3` or `eac3` for the new readers.
- AVI video: timescale and frame step come from the stream header (`dwRate`/`dwScale`, reduced). Chunks are found through the OpenDML
  indexes when there are any, else `idx1`, else by walking the `movi` lists. Empty chunks are not samples; the picture before them
  lasts longer. MPEG-1, MPEG-2 and MPEG-4 pictures get their shown order from their picture types, H.264 from its picture order
  count; `dts` is moved back so it never passes `pts`. A DivX packed bitstream (marked by the `DivX...p` user data) is split into one
  sample per picture and its placeholder chunks are dropped. H.264 samples are converted from start codes to 4-byte lengths.
- AVI sound: MP3, MP2, AC-3 and PCM are read as one byte stream across the chunks and cut into their own frames, so chunk borders
  and AVI block timing do not matter. `Sample.offset` is the place in that stream.
- AC-3 and E-AC-3 decode to the channel order above. Layouts that have no place in it (2.1, 2/1, 2/2 and any other layout with a
  low-frequency channel short of 5.1) are decoded as 5.1 with silent channels where the stream has none.
- AMR and AC-3 output is moved to where FFmpeg's decoders put it (Apple's decoders give it 40, 95 and 256 frames later).
- MPEG-4 part 2 without `ABSENT_XVID` is decoded by the system, which only knows Simple Profile. A stream that uses B-frames,
  quarter-pixel motion or custom matrices is read and probed but `VideoDecoder::open` fails with a message that says so. With
  `ABSENT_XVID` every MPEG-4 part 2 stream goes through Xvid (next section).
- JPEG pictures that are not 4:2:0 are refused by VideoToolbox; they are decoded through ImageIO and come out as 8-bit 4:4:4 BT.709
  limited-range frames (backend `imageio`).
- Windows Media (ASF: WMA, WMV) is recognised and refused with its own message; nothing on a Mac decodes it.
- Alpha: `Track.alpha` is set for VP8 and VP9 in Matroska/WebM whose blocks carry an alpha picture (BlockAdditional 1);
  `Reader::readAlpha` gives that picture's packet. `VideoDecoder` decodes both with libvpx and, for ProRes 4444 with an alpha channel,
  asks VideoToolbox for it. The frame's pixel buffer stays the usual two-plane YCbCr buffer; the 8-bit alpha plane rides on it as the
  `absent.alpha` attachment, `Picture.alpha` points at it, and BGRA/RGBA output (`copyBGRA`, `frames-out`) carries it, straight
  (not premultiplied), scaled with the picture. I420 and RGB24 output have no alpha.
- A sample the decoder rejects does not end the stream: `VideoDecoder::frame` returns the nearest earlier frame that decoded, with the
  asked frame's time, as FFmpeg's concealment shows a repeat.
- A Matroska video codec no decoder here knows is kept as a track named by its VFW fourcc or the first letters of its codec ID, so
  probe reports its name, start and length; `VideoDecoder::open` fails with `video codec not supported by this decoder`.

## MPEG-4 part 2 through Xvid (`ABSENT_XVID`, `src/opt/xvid_decode.cpp`)

- Built when xvidcore is found (FFAMP_DEPS from `third_party/build_mac.sh`, or Homebrew's `xvid` for developer builds). Then every
  `mp4v` track with an `esds`, Simple and Advanced Simple Profile alike, is decoded by Xvid (backend `xvid`): one decoder, one look.
  B-frames, quarter-pixel motion, GMC (S-VOPs), MPEG and custom quantiser matrices, interlaced field DCT and field motion in P-VOPs.
- The decoder is opened with the track's width and height; the VOL headers come from the `esds` DecoderSpecificInfo (MP4, and the
  descriptor the Matroska and AVI readers build). Each sample goes in as stored; DivX packed samples are already split by the AVI
  reader.
- Xvid runs in its low-delay packed mode (a DivX user data marker fed after the headers switches it on; DivX markers inside the
  stream are renamed in the copy it reads so they cannot switch it off). In that mode every VOP comes out when it is decoded, so a
  picture always carries the time of its own sample, nothing is held back (`drain` has nothing to give) and a jump needs no flush.
  B-VOPs right after the key frame a jump starts from can lack a reference and come out wrong or not at all; they are shown before
  that key frame, so they are never the frame asked for (a jump to one of them starts at the key frame before).
- A jump destroys the Xvid instance; the next sample starts a new one with the headers again.
- In a packed AVI an N-VOP that follows no packed pair (Xvid's encoder writes one at the end when it was flushed with B-frames
  waiting) is a slot with no picture: the picture before it lasts longer, as FFmpeg shows it.
- `xvid_global(XVID_GBL_INIT)` runs once per process (`std::call_once`); instances share nothing else.
- Each sample is copied into a buffer with 64 zero bytes behind it: Xvid's bit reader (built with its default
  `XVID_SAFE_BS_TAIL`) stops at the end of the stated length but still reads the next two words.
- Xvid takes a stream without an `XviD` user data string as bug free; FFmpeg's workarounds for old Xvid builds (no user data and
  the `XVID` fourcc) and for DivX 5 quarter-pixel chroma before build 1814 are not applied.
