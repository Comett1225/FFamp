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
| Windows Media Video 7, 8, 9, VC-1, Microsoft MPEG-4 v1 to v3 | `WMV1`, `WMV2`, `WMV3`, `WVC1`, `WMVA`, `MPG4`, `MP42`, `MP43` (every reader gives these names for the fourccs that mean the same codec: `DIV3`, `DIV4`, `DIV5`, `DIV6`, `MPG3`, `DVX3`, `AP41`, `COL0`, `COL1` are `MP43`; `DIV2` is `MP42`; `MP41` is `MPG4`; `GXVE` is `WMV2`) | `bmih` | the stream's BITMAPINFOHEADER (40 bytes, little-endian) followed by its codec data (WMV2: 4 bytes of stream settings; WMV3: the 4-byte sequence header; WVC1: sequence header and entry point); Matroska `V_MPEG4/MS/V3` gives `MP43` with no config |
| Windows Media Audio 1, 2, 9 Pro, 9 Lossless, 9 Voice | `wma1`, `wma2`, `wmap`, `wmal`, `wmas` | `wfex` | the stream's WAVEFORMATEX (18 bytes, little-endian; WAVEFORMATEXTENSIBLE when the tag is 0xFFFE) followed by its codec data; one sample is one ASF media object |

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

## ASF: Windows Media files (.wma, .wmv, .asf)

- `Reader::format()` is `asf`; probe's format name is `asf` and every time base is 1/1000.
- Tracks: every audio and video stream the header describes (Stream Properties objects, also those inside Extended Stream Properties),
  in header order; `Track.id` is the ASF stream number. Script command, image, file and binary streams are skipped. A stream with no
  complete media object is kept with no samples.
- Samples: one per media object, rebuilt from its payloads across packets: error correction data, every length type, a stated packet
  length, single and multiple payloads, compressed payloads (one replicated byte: several small objects in one payload), padding. A
  media object whose pieces do not join up, or that the end of the file cuts off, is left out (FFmpeg passes on the part that is
  there). `Sample.offset` is the place in the stream's own byte sequence, not in the file.
- Times: milliseconds (`timescale` 1000): presentation time minus the File Properties preroll plus the stream's time offset.
  `mediaStart` and `delay` are both the earliest time, so a sample is shown at its own `pts`.
- Video whose times rise in decode order keeps them as pts and dts, except H.264, whose display order comes from the picture order
  counts (FFmpeg writes decode times into ASF); times that do not rise are presentation times and get a made-up dts no later than pts,
  as in Matroska. Durations are the gaps in display order; the last one is the Extended Stream Properties frame time.
- Key frames: the key-frame bit of the payloads. A video stream with no key-frame bit at all takes them from the Index or Simple Index
  objects (in the indexed packet, the last media object that starts there at or before the entry's time) and H.264 from its IDR
  pictures; the first picture is always one. Seeking is over all samples, so it works with or without an index.
- H.264 stored with start codes is turned into 4-byte lengths (access unit delimiters and filler dropped) with an `avcC` built from
  the parameter sets in the codec data and the stream. MPEG-4 part 2 (`M4S2`, `MP4S` and the AVI fourccs) gets an `esds` from the
  codec data, or from the headers in front of the first picture.
- Sound: MP3, MP2, AC-3 and PCM are read as one byte stream and cut into their own frames (PCM in 4096-frame packets) with the sample
  rate as `timescale`, as in AVI; the first frame starts at the first media object's time. Every other sound codec keeps one sample per
  media object, `sync` on all of them, duration = the gap to the next.
- Audio spread error correction is undone while the pieces are listed: in each whole group of span x virtual packet length bytes of
  a media object, chunk `k` of the output is stored chunk `(k mod n) x span + k / n`, `n` = virtual packet length / chunk length.
  Bytes after the last whole group stay as stored.
- DRM: a Content Encryption, Extended Content Encryption or Advanced Content Encryption object, or a stream marked encrypted, makes
  `Reader::open` fail with `this Windows Media file is protected by DRM (digital rights management) and cannot be played`.
- Tags: Content Description (`title`, `artist`, `copyright`, `comment`, `rating`), Extended Content Description (`WM/AlbumTitle` album,
  `WM/Year` date, `WM/Genre` genre, `WM/TrackNumber` track, `WM/AlbumArtist` album_artist, `WM/EncodingSettings` encoder, others
  under their own names; text, numbers and booleans, not byte values), `creation_time` from File Properties. `Track.language` is the
  Language List entry as stored (RFC 1766, `en-us`); ffprobe shows ISO 639-2 (`eng`).
- `statedSeconds()` is the play duration minus the preroll, except for broadcast files and files whose data object runs past the end.
- Probe names `wmav1`, `wmav2`, `wmapro`, `wmalossless`, `wmavoice`, `wmv1`, `wmv2`, `wmv3`, `vc1`, `msmpeg4v1`, `msmpeg4v2`,
  `msmpeg4v3`, from ASF, AVI and Matroska. Windows Media Audio 1 and 2 and every Windows Media video codec decode (sections below);
  until their codec rounds land, `AudioDecoder::open` fails with `this sound codec (Windows Media Audio 9 Professional) is not decoded
  yet` (each with its own name).
- H.263 and Motion JPEG in ASF get their FFamp codes (`h263`, `jpeg`) and decode. Other sound tags keep a track named by the tag in
  hex (`0130`), other video fourccs a track named by the fourcc.

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
- Windows Media files open through the ASF reader (next section).
- Alpha: `Track.alpha` is set for VP8 and VP9 in Matroska/WebM whose blocks carry an alpha picture (BlockAdditional 1);
  `Reader::readAlpha` gives that picture's packet. `VideoDecoder` decodes both with libvpx and, for ProRes 4444 with an alpha channel,
  asks VideoToolbox for it. The frame's pixel buffer stays the usual two-plane YCbCr buffer; the 8-bit alpha plane rides on it as the
  `absent.alpha` attachment, `Picture.alpha` points at it, and BGRA/RGBA output (`copyBGRA`, `frames-out`) carries it, straight
  (not premultiplied), scaled with the picture. I420 and RGB24 output have no alpha.
- A sample the decoder rejects does not end the stream: `VideoDecoder::frame` returns the nearest earlier frame that decoded, with the
  asked frame's time, as FFmpeg's concealment shows a repeat.
- Microsoft MPEG-4 v1 to v3, WMV7 and WMV8 in AVI and in Matroska (`V_MS/VFW/FOURCC`) get the names above with configType `bmih`, as
  in ASF. In AVI their key frames are the I-pictures, read from each chunk's picture header (the index's key flags are not used: old
  DivX 3 files often have no index or mark every chunk). These codecs have no B-pictures, so shown order is stored order. `DIV5` and
  `DIV6` are Microsoft MPEG-4 v3, as FFmpeg maps them, not MPEG-4 part 2.
- VC-1 in AVI and in Matroska (`V_MS/VFW/FOURCC`) keeps its fourcc (`WMV3`, `WVC1`, `WMVA`) with configType `bmih`, as in ASF. In
  AVI its key frames (I pictures and field pairs that start with an I field) and its shown order (a B or BI picture is shown before
  the anchor stored ahead of it) come from the picture types, as for MPEG-4; the index's key flags are not used.
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

## VC-1: WMV3, WVC1 and WMVA (`src/vc1_*.cpp`, backend `vc1`)

- FFamp's own software decoder, based on FFmpeg's VC-1 decoder (the files are listed in `docs/NOTICE.md`), behind `SoftDecoder`
  (`openVc1`). `WMV3` is Simple, Main and Complex profile; `WVC1` and `WMVA` are Advanced profile: progressive, interlaced frame and
  interlaced field pictures, slices, I, P, B, BI and skipped pictures, 1 and 4 motion vectors, intensity compensation, range
  reduction, overlap smoothing, the loop filter, quantiser and transform switching per macroblock.
- The track needs configType `bmih` (the ASF, AVI and Matroska readers give it for these three fourccs: the BITMAPINFOHEADER and the
  codec data behind it). `WMV3` takes its sequence header from the codec data and its size from the header; `WVC1` and `WMVA` take
  the sequence header and entry point from the start-code framed codec data, and the coded size from the entry point. A sample may
  carry start codes (frame, field, slice, entry point, sequence header) or be one bare picture; a bare interlaced field pair is split at
  its field start code.
- Complex profile streams with X8 intra pictures (`kelsy_300.wmv`, `sample-wmv9-complex-CP.wmv`) are refused with `this VC-1 stream
  uses X8 intra pictures (Windows Media Video 9 Complex profile), which are not decoded yet`; Complex profile without X8 (the
  `sb-adv-q94.avi` sample) decodes, with the simple IDCT those streams ask for. Sprite streams (`WMVP`, `WVP2`) are refused.
- Every sample's picture comes out with that sample's time: B-pictures need no reorder delay because the reader's presentation times
  put them in order. Output is 8-bit 4:2:0 at the coded size, cut to the track size when the track is smaller.
- Frame threads: a B or BI picture (nothing refers to it) is decoded on one of two worker threads, from a copy of the decoder state
  and the shared reference pictures, while the caller's thread goes on with the next anchor. Its picture is handed to the sink at a
  later `decode` (on the caller's thread) or at `drain`. A Simple or Main profile BI picture still sets the rounding control of the
  anchors that follow, as in a straight decode. I and P pictures, and Advanced profile samples that carry a sequence header or entry
  point, are decoded on the caller's thread. A jump waits for the workers and drops their pictures.
- A B-picture with no earlier reference (right after a jump) and a picture that does not decode return `false`, so `VideoDecoder`
  shows the nearest earlier picture as FFmpeg's concealment does; a jump destroys the decoder state and starts again at a key frame.
- Frame buffers come from a small pool owned by the decoder: a picture handed to the sink stays valid until the next `decode`.
- Bits and VLC tables: the shared `BitReader` and `Vlc` of `src/bits.h` and `src/vlc.h` (the WMA framework), with the few VC-1
  readers (unary, 0-1-2 codes) on top in `src/vc1.h`; the VC-1 tables are built from FFmpeg's code and length tables.

## Audio framework for the Windows Media family (`src/bits.h`, `src/vlc.h`, `src/mdct.h`, `src/wma.h`)

- `BitReader`: most significant bit first, `read(n)` for 0 to 32 bits, `peek`, `skip`, `align`, `left` (may go negative). Reading past
  the end gives zero bits and never touches memory outside the buffer; a third constructor argument limits the stated length in bits
  while the bytes behind it stay readable (the bit reservoir).
- `Vlc`: lookup tables of any depth (`rootBits` per level, longer codes in sub-tables). `build` takes lengths and codes (value
  right-aligned, length 0 = unused) and optional symbols; `buildInOrder` assigns codes in the order given from lengths alone (the
  canonical order FFmpeg's tables are stored in). `read` returns the symbol, or -1 for a code that is not in the table.
- `InverseMdct(bits, scale)`: N = 2^bits coefficients; `half` gives the N middle samples, `full` all 2N, scaled by `scale` (a negative
  scale flips the sign as FFmpeg's does). Portable C++ (radix-2 FFT over split real and imaginary arrays); one instance per decoder,
  not shared between threads. `sineWindow(bits)` is the rising half of a sine window of 2^bits values (bits 5 to 13).
- `wma::frameLengthBits(rate, version, flags)` (versions 1 to 3, the Pro flags included), `wma::largeValue`, `wma::runLevel` (the
  run-level coefficient coder of WMA 1/2 and Pro, both escape versions) and the tables of `src/wma_tables.cpp`.
- `PacketDecoder` (src/audio_codec.h) gained what the WMA decoders need from `AudioDecoder`:
  - `counts(reader, track, frames)`: the exact number of frames each packet decodes to (the last packet includes what `finish` gives).
    `AudioDecoder` then places packet `i` at the first packet's time plus the frames before it; container times after the first are
    not used, so millisecond times (ASF, Matroska) do not move the sound.
  - `skip`: frames at the start of the stream that FFmpeg's decoder hides (its decoder delay). `AudioDecoder` plays them as silence,
    so sample `k` of the output stays at the track's start plus `k` and the rest equals FFmpeg's output. A container that states its
    own start skip (`mediaStart` after the first packet, as Matroska's CodecDelay) replaces it, as in FFmpeg.
  - `save` / `load`: the bitstream state that a restart a few packets back cannot rebuild (for WMA the noise generator's position,
    the bit reservoir and the block sizes). `AudioDecoder` keeps one mark per 20-second run, taken while decoding passes it; a jump to
    a run with no mark loads the nearest earlier one and brings the decoder forward with `skim`.
  - `skim(packet)`: advance the state through a packet without making sound (WMA parses the packet and moves the noise generator but
    skips the transforms).

## Windows Media Audio 1 and 2 (`wma1`, `wma2`, `src/wma_decode.cpp`)

- Sources: ASF (`wfex`, one sample per media object), AVI (`wfex` from `strf`, one sample per chunk, timescale = sample rate, times from
  the byte position and the average bytes per second) and Matroska (`A_MS/ACM`, the CodecPrivate WAVEFORMATEX as `wfex`; WAVEFORMATEXTENSIBLE
  is followed to its sub-format).
- Based on FFmpeg's decoder (`docs/NOTICE.md`) and checked against it: float output, mono or stereo, 1 to 50,000 Hz, frames of 512,
  1024 or 2048 samples, every block size, variable block lengths, bit reservoir across superframes, LSP and VLC exponents, noise
  coding, mid/side stereo. Each packet is cut into `block_align` superframes; a shorter remainder is dropped, as in FFmpeg.
- Frame counts: without the bit reservoir one frame per superframe; with it the count in each superframe's header (the first
  superframe of the stream one fewer). `AudioDecoder` therefore reads every packet once when a reservoir stream is opened. The last
  packet adds the final overlap (FFmpeg's end flush), so lengths equal FFmpeg's to the sample.
- Start: the first two frames are silent (FFmpeg hides them); with a Matroska CodecDelay only that is hidden, as in FFmpeg.
- Jumps restart the decoder three packets and at least two frames early; noise-coded streams also load a mark (see above), so every
  read equals the straight decode bit for bit. A packet that fails to decode is silence of its own length.
- On purpose unlike FFmpeg: a superframe whose header ends no frame (count 0) is added to the reservoir by its length in bytes
  (FFmpeg adds eight times the length and loses the frame that continues there), and the first superframe after a restart that ends
  no frame keeps its tail for the next one. Neither occurs in the sample files.

## The H.263-family core (`src/h263.h`, `src/h263.cpp`)

- What the macroblock decoders of this family share, based on FFmpeg's `MpegEncContext` code (files in `docs/NOTICE.md`): `Mpv` holds
  the macroblock geometry, the DC, AC and coded-block prediction arrays (kept across pictures, as FFmpeg keeps them), block indexes,
  H.263 motion vector prediction, the MPEG-4 AC prediction, the H.263 intra dequantiser, 16x16 half-pel motion compensation with edge
  emulation, the H.263 loop filter, and the H.263 and Microsoft MPEG-4 VLC and run-level tables (`src/msmpeg4_tables.cpp`).
- It reuses what VC-1 already has instead of copies: the bit reader and VLC tables (`src/bits.h`, `src/vlc.h`; `Vlc::lookup` decodes
  from a cached 64-bit window so the run-level loop reads code and sign with one load), `vc1::Bits`, the frame pool and picture
  buffers (`vc1::FramePool`, now shared, 64-pixel margins), the half-pel copy and average functions, edge emulation, the simple IDCT
  family and the Microsoft MPEG-4 DC, intra pattern and scan tables of `src/vc1_tables.cpp`.
- IDCT: FFmpeg on arm64 runs its NEON simple IDCT for these codecs, which computes rows without the C version's shortcut for rows
  that hold only a DC value; `vc1::neonSimpleIdctPut` and `neonSimpleIdctAdd` give exactly its results (NEON intrinsics on arm64, the
  same arithmetic in plain C++ elsewhere). WMV8 uses its own IDCT (`wmv2IdctPut`, `wmv2IdctAdd` in `src/intrax8.cpp`).

## Microsoft MPEG-4 v1 to v3, WMV7 and WMV8 (`src/msmpeg4.cpp`, backends `msmpeg4v1`, `msmpeg4v2`, `msmpeg4v3`, `wmv1`, `wmv2`)

- One decoder for the five versions, based on FFmpeg's (`docs/NOTICE.md`), behind `SoftDecoder` (`openMsmpeg4`). The track's width
  and height set the macroblock grid; WMV8 reads its 4 bytes of codec data (bit rate, loop filter, mspel, adaptive transform types,
  X8 pictures, slices) from behind the BITMAPINFOHEADER; the others need no codec data.
- I and P pictures, every VLC set of each version, the three escape kinds, per-macroblock run-level tables, inter-intra DC prediction
  (WMV7 at low bit rates), flip-flop rounding, slices, WMV8 skip maps, quarter-pel mspel motion, 8x4 and 4x8 transforms, the loop filter
  and WMV8 X8 intra pictures (`src/intrax8.cpp`, the J-frames).
- Every sample gives one picture with its own time. A WMV8 P-picture in which every macroblock is skipped carries no picture: the one
  before is shown again with this sample's time (FFmpeg drops it). A picture whose macroblocks stop decoding part way keeps what was
  decoded and fills the rest from the picture before (grey at the start); FFmpeg conceals with its error resilience, so such pictures
  differ. A jump starts a fresh decoder at the key frame before; the prediction state FFmpeg keeps across pictures is rebuilt by every
  I-picture, so jumps give the straight decode.
