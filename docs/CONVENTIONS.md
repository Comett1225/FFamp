# Conventions shared by every reader, decoder and writer

Every reader fills `absent::Track` the same way, so any decoder or writer works with any container.

## Track.codec (fourcc) and Track.config

| Stream | codec | configType | config bytes |
|---|---|---|---|
| H.264 | `avc1` (also for `avc3` input) | `avcC` | AVCDecoderConfigurationRecord (samples are length-prefixed NAL units, 4-byte lengths unless avcC says otherwise) |
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
| PCM | `lpcm` | 0 | empty; `bits` (8, 16, 24, 32, 64), `floating`, `bigEndian` describe it; 8-bit is unsigned unless `bigEndian` is set (QuickTime `twos` and CAF 8-bit are signed; AIFF readers flip the sign bit while reading), others signed; 64-bit integers too |
| G.711 A-law / mu-law | `alaw` / `ulaw` | 0 or `wfex` | empty, or the WAVEFORMATEX it came with |
| QuickTime IMA ADPCM | `ima4` | 0 | empty; blocks of 34 bytes (64 samples) per channel |
| TTA (True Audio) | `TTA1` | `TTA1` | the 22-byte TTA1 header; Matroska builds it as FFmpeg does (format 1, channels, bit depth, rate, the segment duration in frames) |
| Sound named by a WAVE format tag: Microsoft ADPCM (0x0002), IMA ADPCM (0x0011, 2 to 5 bits), Yamaha ADPCM (0x0020), IMA DK4 (0x0061), IMA DK3 (0x0062), G.726 (0x0014, 0x0040, 0x0045, 0x0064), G.722 (0x028F) | `ms` and the 16-bit tag, as QuickTime names them (`ms\0\x02` = 0x6D730002) | `wfex` | the WAVEFORMATEX and its codec data; QuickTime takes it from the atom of that name inside `wave`, CAF and QuickTime files without one get a made-up one from their description |
| Windows Media Video 7, 8, 9, VC-1, Microsoft MPEG-4 v1 to v3 | `WMV1`, `WMV2`, `WMV3`, `WVC1`, `WMVA`, `MPG4`, `MP42`, `MP43` (every reader gives these names for the fourccs that mean the same codec: `DIV3`, `DIV4`, `DIV5`, `DIV6`, `MPG3`, `DVX3`, `AP41`, `COL0`, `COL1` are `MP43`; `DIV2` is `MP42`; `MP41` is `MPG4`; `GXVE` is `WMV2`) | `bmih` | the stream's BITMAPINFOHEADER (40 bytes, little-endian) followed by its codec data (WMV2: 4 bytes of stream settings; WMV3: the 4-byte sequence header; WVC1: sequence header and entry point); Matroska `V_MPEG4/MS/V3` gives `MP43` with no config |
| DNxHD / DNxHR | `AVdn`, `AVdh` as found | 0 | empty |
| QuickTime Animation | `rle ` | 0 | empty; `bits` is the sample description's depth (16, 24 and 32 are decoded) |
| PNG | `png ` (also for AVI `MPNG`) | 0 | empty; one PNG file per sample |
| Ut Video | the fourcc as found (`ULY0`, `ULY2`, `ULY4`, `ULRG`, `ULRA`, `ULH0`, `ULH2`, `ULH4`, `UQY0`, `UQY2`, `UQRG`, `UQRA`; the `UM` pack modes too) | `bmih` | the BITMAPINFOHEADER and the codec's 16 (or 8) bytes behind it |
| FFV1 | `FFV1` | `bmih` (AVI, Matroska `V_MS/VFW/FOURCC`) or `FFV1` (Matroska `V_FFV1`) | the BITMAPINFOHEADER with the configuration record behind it, or the record alone; versions 0 and 1 have none |
| HAP | `Hap1`, `Hap5`, `HapY`, `HapM`, `HapA` | 0 | empty |
| v210 and uncompressed video | `v210`; the raw YUV fourccs (`I420`, `IYUV`, `YV12`, `NV12`, `NV21`, `YUY2`, `YUYV`, `yuvs`, `UYVY`, `2vuy`, `HDYC`, `Y42B`, `Y800`, `GREY` and the AVI list below); `raw ` (QuickTime RGB, `bits` 16, 24 or 32); `DIB ` (BI_RGB in AVI and Matroska) | `bmih` for `DIB ` and AVI | the BITMAPINFOHEADER and its palette: bit count 1, 2, 4, 8 (palette), 15, 16, 24 or 32, rows bottom-up when its height is positive, padded to 4 bytes |
| DTS | `dts ` | 0 | empty; one frame per sample, starting with the core sync (16-bit big- or little-endian or 14-bit words); Matroska `A_DTS*`, MP4 (`mp4a` with object type 0xA9 to 0xAC, `dtsc`, `dtsh`, `dtsl`, `dtse`), AVI and WAV (tag 0x2001, cut into core frames) |
| TrueHD / MLP | `mlpa` | 0 | empty; a sample holds one or more access units (Matroska `A_TRUEHD` and `A_MLP`, MP4 `mlpa`) |
| WavPack | `wvpk` | `wvpk` | the Matroska CodecPrivate (2-byte version, may be empty); a sample is a Matroska WavPack frame: the 4-byte sample count, then for each block its flags, CRC, size (multi-block frames only) and data |
| Windows Media Audio 1, 2, 9 Pro, 9 Lossless, 9 Voice | `wma1`, `wma2`, `wmap`, `wmal`, `wmas` | `wfex` | the stream's WAVEFORMATEX (18 bytes, little-endian; WAVEFORMATEXTENSIBLE when the tag is 0xFFFE) followed by its codec data; one sample is one ASF media object |

## Timing

- `timescale`: ticks per second for `Sample.pts/dts/duration`. Audio tracks should use the sample rate when the container allows it.
- `mediaStart`: first presented tick (edit list start, Opus pre-skip, MP3 encoder delay + 529, Matroska CodecDelay). Samples before it are
  decoded but not shown.
- `mediaEnd`: tick where presentation ends (-1 = until the last sample ends). Used for encoder padding (LAME tag, Ogg end granule,
  edit list duration).
- `order`: indexes of shown samples sorted by pts; `syncs`: key frames in decode order.
- Matroska and TS have no stored dts: readers synthesise a non-decreasing dts not above pts.
- Matroska Vorbis: the first packet lasts (its window + the next packet's window) / 4, as FFmpeg's Vorbis parser times it, so a file
  without CodecDelay (mkvmerge) starts with that packet's silent frames, as the app's FFmpeg reading does.

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
- Packets reach the file in decode-time order whatever order they are handed in, as FFmpeg's muxer interleaves them: a writer holds
  them until every track has one waiting (or 10 s of time or 64 MB are waiting, or `finish`) and writes the earliest first. Callers
  may feed sound ahead of an encoder's delayed pictures; a Matroska cluster then holds the sound of its own time, so sound survives
  a seek to a cue.
- `finish` moves the data of `<output>.part` forward in place by the size of the header and renames the file: the drive needs the
  output's size plus its header, not twice the size.
- Colour labels: `TrackSpec.color` defaults to BT.709 limited with `known` set, what the encoders write. Copies take `Track.color`,
  so a source without labels gives a track with no `colr` box and no Matroska `Colour` element (ffprobe then reads the stream's own
  VUI, as from the source). `colr` is written only when primaries, transfer and matrix are all given, as in FFmpeg; Matroska leaves
  out each unspecified (2) value.
- `Writer::tags` takes a file's tags (the copy tools pass `Reader::tags()`). MP4, MOV and M4A write title, artist, album_artist,
  composer, album, date, comment, genre, copyright, grouping, lyrics and description as iTunes items (`udta/meta/ilst`) and location
  as `©xyz`; Matroska writes the title into Info and the others as global SimpleTags in capitals, without creation_time and encoder.
  The MP4 reader reads `©xyz` as location.
- MP4 also holds MPEG audio (`mp4a`, object type 0x6B, 0x69 at 24 kHz and below), Vorbis (`mp4a`, object type 0xDD, the Xiph-laced
  headers as decoder specific info; the reader takes them back as `vorb`), AC-3 and E-AC-3 (`ac-3`/`ec-3` with `dac3`/`dec3` made
  from the first packet as FFmpeg makes them; E-AC-3 packets are not joined into six-block samples) and Motion JPEG, MPEG-1 and
  MPEG-2 video (`mp4v`, object types 0x6C, 0x6A and 0x61). MOV takes MP3 as `.mp3` (sound description version 1), AC-3 and E-AC-3
  inside `wave`, Motion JPEG, MPEG-1 and MPEG-2 video as `jpeg`, `m1v ` and `m2v1`, and
  refuses Vorbis and MP2. M4A refuses MP3, MP2, Vorbis and E-AC-3, as FFmpeg's `ipod` muxer does.
- Matroska also holds MPEG-4 part 2 (`V_MPEG4/ISO/ASP`, the decoder specific info as CodecPrivate), Motion JPEG (`V_MJPEG`), MPEG-1 and
  MPEG-2 video (`V_MPEG1`, `V_MPEG2`), MP2, AC-3 and E-AC-3. Video tracks get a DefaultDuration: the steps between shown frames that lie within 5 % of the median step,
  averaged, and snapped to a standard rate (23.976 to 120) within 0.2 %. Rotation is written as ProjectionPoseRoll, the negative of
  `rotation` in (-180, 180], as FFmpeg writes a display matrix, and the reader turns it back into `rotation`.
- MP4/MOV stretch the last sample of a video track to the end of `length` (`Writer::trim`) when the track would end earlier, so a held
  last frame keeps its time with encoders that hand out no packet durations (x264, Media Foundation).

## Presentation delay and file clocks (`Track.delay`)

- `delay` (track timescale) is the time on the file's own clock at which the tick `mediaStart` is shown. A sample is shown at
  `pts - mediaStart + delay`. Tracks of one file are kept in sync by their difference in `delay`; a consumer that wants the file to start
  at zero subtracts the smallest `delay` of all tracks (what the app does with ffprobe's format `start_time`).
- MP4/MOV: leading empty edits (media_time -1) add up to `delay`. The first media edit sets `mediaStart` and, when it has a duration,
  `mediaEnd`; edits that continue exactly where the previous one ended are joined to it. Samples at or after `mediaEnd` are left out of
  `order`. In a video track `mediaStart` is the first composition time inside the edit, so an edit at media time 0 on a track whose
  first picture is shown later starts that picture at the edit's start, as FFmpeg does (with no edit list the picture keeps its time).
- MP4/MOV with several separate media edits: the segments are joined into one timeline, each one starting where the one before it
  ends (`mediaEnd` is the end of the last). Video keeps, for each segment, the samples from the key frame before it up to its last
  shown sample; the samples among them that the segment does not show are decoded but not shown: their `pts` are moved below
  `mediaStart` (unique, rising in decode order, never a shown time). Sound keeps the packets that start inside each segment (the
  first segment also its run-in), as FFmpeg does. A segment's first shown sample is placed at the segment's start. A stream copy
  keeps those samples before the start of its edit, where readers that go by time (FFamp, AVFoundation) do not show them; FFmpeg
  goes by its index and shows fewer frames of such a copy (its own copy of the source is no better).
- Fragmented MP4: every fragment sample's `pts` gets the track's largest negative composition offset added (FFmpeg's `dts_shift`),
  so negative offsets show the video when FFmpeg shows it.
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
- Stream copies (`trimCopy`, `rewrap`, `stripAudio`, `merge`, `extractAudio` by copy) keep the source's tags, rotation and colour labels
  (none when the source has none); `merge` takes the tags of the video file.

## Decoded sound (`AudioDecoder`)

- Channel order by count, for the mixer and the WAV writer and, on the Mac, for every codec: 3 = FL FR FC, 4 = FL FR FC BC (WAV mask
  0x107), 5 = FL FR FC BL BR, 6 = FL FR FC LFE BL BR, 7 = FL FR FC LFE BC SL SR (WAV mask 0x70F), 8 = FL FR FC LFE BL BR SL SR.
  FLAC comes out in its own file order, which is this order (4-channel FLAC and Opus are really quad: all four channels are kept).
- Where the stream states its layout, `AudioDecoder::layout()` gives it as an FFmpeg channel mask and the mixer places each channel by it:
  the WAVEFORMATEXTENSIBLE mask of WAV, AVI and ASF (PCM and the WAVE-tag codecs), the MOV `chan` box (bitmap, descriptions and the
  layout tags whose order is the mask's), the FLAC `WAVEFORMATEXTENSIBLE_CHANNEL_MASK` comment (raw FLAC and Matroska), the WMA Pro and
  Lossless masks, the DTS, TrueHD and WavPack layouts. 4-channel FLAC, Vorbis and Opus (family 1) are quad (`0x33`); 3-channel PCM
  without a mask and Opus family 255 take FFmpeg's guess for the count (2.1, so the third channel is LFE).
- The mixer is swresample's matrix: centre and surround at -3 dB unless the stream states its own levels (AC-3 and E-AC-3, read from
  the first frame on every system: the bit stream info, the alternate syntax of bsid 6, E-AC-3 mixing metadata with its LFE level and
  preferred Lt/Rt or Pro Logic II downmix, which only stereo output uses), LFE dropped unless a level is stated, a back centre to mono
  at `surround * sqrt(0.5)`. `PcmReader::mixPeak()` is the largest row sum of the reader's matrix; 16-bit output divides by it when it
  is above 1, as swresample normalises a downmix for integer output (the `pcm --format s16le` command does).
- Off the Mac every decoder gives FFmpeg's order, which differs from the order by count where FFmpeg's layout does: 6.1 ALAC and AAC
  come out as FL FR FC LFE BL BR BC (`6.1(back)`), AAC with a program config element in FFmpeg's order for its layout. Such decoders
  state their layout as an FFmpeg channel mask (`AudioDecoder::layout()`, 0 when the order is the one by count), and `PcmReader`'s
  mixer places each channel by it, so a 6.1 file mixes to stereo with every channel on its own side on every system.
- A gap between packets that the reader keeps, a packet that cannot be read and a packet the decoder rejects are played as silence of
  the same length, so the sound after them stays at its own time. Gaps of any length are kept (up to an hour of silence in all), as
  FFmpeg's `aresample=async=1` fills them; with a decoder whose output lags its input (Media Foundation AAC) the silence goes after
  the output still inside the decoder.
- `Writer::trim(track, skip, 0)` means nothing is shown: an encoder that was given no sound writes a file that decodes to 0 frames.
- `Mp4Writer` writes version 1 (64-bit) `mvhd`, `tkhd`, `mdhd` and `elst` boxes when a length does not fit in 32 bits.
- `PcmReader::mixPeak()` is the largest row sum of its channel mix (2.414 for 5.1 to stereo, 1 when nothing is mixed). Float output is
  not scaled, as FFmpeg's; 16-bit output is divided by it when it is above 1, as FFmpeg's resampler normalises its matrix for integer
  sample formats (`pcm --format s16le` does this, and so must anything else that turns `PcmReader` output into 16-bit samples).

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
  `msmpeg4v3`, from ASF, AVI and Matroska. Every Windows Media audio and video codec decodes (sections
  below).
- H.263 and Motion JPEG in ASF get their FFamp codes (`h263`, `jpeg`) and decode. Other sound tags keep a track named by the tag in
  hex (`0130`), other video fourccs a track named by the fourcc.

## AVI, CAF, AC-3, AMR and older video (formats round)

- `Reader::format()` is `avi`, `caf`, `amr`, `ac3` or `eac3` for the new readers.
- AVI video: timescale and frame step come from the stream header (`dwRate`/`dwScale`, reduced); a header without them takes the main
  header's frame period, else 25 frames a second, as FFmpeg does. The step is kept as `Track.frameStep`, and `Track::rate()` and
  probe's `avg_frame_rate` are `timescale / frameStep` (FFmpeg's `avg_frame_rate` for AVI is the header rate), also when empty chunks
  make some pictures last longer. `frameStep` is 0 for every other reader. Chunks are found through the OpenDML indexes when there are any
  (super indexes, nested ones too, and standard indexes; a base offset that repeats its low 32 bits in the high ones is cut to 32 bits,
  as FFmpeg does), else `idx1`, else by walking the `movi` lists. The `idx1` base is the `movi` list, the file start or the distance
  between the first entry and the first chunk the walk finds (FFmpeg's rule), whichever makes the first entry, up to 64 others spread
  over the index and the last one land on their chunk ids; an index that fits none of them is not used. After an index the data
  past the last indexed chunk is walked as well, so an `idx1` that only covers the first RIFF part (a file over 1 GB written without
  OpenDML) or that was cut short still gives every chunk. The walk skips `JUNK`, `ix##`/`##ix`, `idx1` and `indx` chunks, enters
  `LIST rec` groups, takes an odd-sized chunk as padded unless only the unpadded position holds a chunk header, and after bytes that
  are no chunk header searches for the next header whose size fits (FFmpeg's `avi_sync`). A chunk cut off by the end of the file
  keeps the part that is there.
- AVI key frames: MPEG-1/2/4, VC-1, Microsoft MPEG-4, H.263 and Cinepak from their pictures (Cinepak: every strip intra, no partial
  codebook update, no skip vectors); H.264 IDR pictures and the I pictures an index marks as key frames (open GOP, from `idx1`
  `AVIIF_KEYFRAME` or the OpenDML key bit); HEVC every IRAP picture; Motion JPEG, DV and raw video on every chunk; any other codec from
  the index flags (`idx1` flag 0x10, OpenDML bit 31 clear), only the first chunk when an `idx1` marks none (FFmpeg), every chunk when
  there is no index.
- Empty chunks are not samples; the picture before them lasts longer (the last picture too). MPEG-1, MPEG-2 and MPEG-4 pictures get
  their shown order from their picture types, H.264 and HEVC from their picture order counts, sorted between IDR (and HEVC BLA)
  pictures, with empty chunks in between taking no part in the order (each shown picture gets one of the places its group of pictures
  holds); `dts` is moved back so it never passes `pts`. A DivX packed bitstream (marked by the `DivX...p` user data) is split into one
  sample per picture and its placeholder chunks are dropped. H.264 and HEVC stored with start codes are converted to 4-byte lengths
  (delimiters and filler dropped) with an `avcC` / `hvcC` built from the parameter sets; stored with lengths and an `avcC` / `hvcC`
  record behind the BITMAPINFOHEADER (what `ffmpeg -c copy` writes from MP4), the chunks are kept as they are and the record is the
  config.
- AVI fourccs: every video fourcc FFmpeg's `riff.c` maps to MPEG-4 part 2, H.264, HEVC (`HEVC`, `H265`, `HEV1`, `HVC1`, `DVHE`), H.263,
  Motion JPEG, DV, MPEG-1 and MPEG-2 (`dvhd`, `dvh1` and `CDVH` give `dvh6`, or `dvhp` at 720 lines). `BI_RGB` gives codec `DIB `, a
  raw YUV fourcc keeps its own name (`YUY2`, `YUYV`, `YUNV`, `V422`, `VYUY`, `Y422`, `YVYU`, `UYVY`, `UYNV`, `UYNY`, `HDYC`, `uyv1`,
  `2Vu1`, `VDTZ`, `auv2`, `cyuv`, `I420`, `IYUV`, `yv12`, `YV12`, `YV16`, `Y42B`, `P422`, `Y41B`, `YUV9`, `YVU9`, `Y800`, `Y8  `,
  `GREY`, `NV12`, `NV21`), Cinepak is `cvid`; all of these get configType `bmih` (the whole format block: BITMAPINFOHEADER and palette)
  and `Track.bits` = biBitCount.
- AVI sound: MP3, MP2, AC-3 and PCM are read as one byte stream across the chunks and cut into their own frames, so chunk borders
  and AVI block timing do not matter. `Sample.offset` is the place in that stream. A dependent E-AC-3 frame joins the packet of the
  frame before it. AAC (tags 0x00FF, 0x1600, 0x1610, 0x706D, 0x4143, 0xA106) takes its AudioSpecificConfig from the codec data
  (0x1610: after the 12-byte HEAACWAVEINFO when its payload type is raw); without one, or with ADTS as the 0x1610 payload, the ADTS
  frames are cut into raw AAC samples (`Sample.offset` past the header) with an AudioSpecificConfig made from the first header.
- DV type-1 files (one `iavs` stream, picture and sound in the same DV frames, chunks with any two-letter suffix such as `00__`): the
  frames are the video track (`dvc ` or `dvcp` by the frame's 525/625 flag, 720 wide) and the sound in their AAUX blocks is a second
  track, 16-bit PCM in two channels at the frame's rate, one sample per frame with the frame's own sample count; reading a sample
  takes the sound out of its DV frame as FFmpeg's DV demuxer does (16-bit, or 12-bit nonlinear widened to 16 bits; 0x8000 and
  0x800 silenced; frames at another rate than the first are left out). Only the first channel pair is read: with 12-bit sound at
  32 kHz FFmpeg gives a second stereo stream from the other half of each frame.
- Not read: palette changes inside the stream (`##pc` chunks), subtitle and data streams.
- AC-3 and E-AC-3 decode to the channel order above. Layouts that have no place in it (2.1, 2/1, 2/2 and any other layout with a
  low-frequency channel short of 5.1) are decoded as 5.1 with silent channels where the stream has none; a lone surround channel
  (2/1, 3/1 with LFE) plays from both back channels at -3 dB, as AudioToolbox and FFmpeg's resampler place it.
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
- Matroska and WebM rotation: Video > Projection with the rectangular type, pitch 0 and yaw 0 or 180 gives `rotation` from its pose
  roll as FFmpeg's display matrix does (roll 90, counter-clockwise, is `rotation` 270, as in MP4; with yaw 180, a mirror, `rotation` is
  the angle FFmpeg reports for that matrix). `MkvWriter` writes ProjectionPoseRoll for a rotated track (`TrackSpec::rotation`).
- Raw MP3 (and MP2, MP1): after junk of any length (a tag with large cover art in the middle of joined files) the reader finds the
  frames again. Frames of the same layer at another sample rate are kept, with durations counted in the first frame's rate; mpg123
  resamples them to that rate, and on the Mac the AudioToolbox converter is rebuilt for their rate with the first rate as its output.
- A Matroska video codec no decoder here knows is kept as a track named by its VFW fourcc or the first letters of its codec ID, so
  probe reports its name, start and length; `VideoDecoder::open` fails with `video codec not supported by this decoder`.

## Raw video and Cinepak (`src/raw_video.cpp`, `src/cinepak_decode.cpp`, backends `rawvideo` and `cinepak`)

- FFamp's own decoders on every system, the Mac and the Android build included, based on FFmpeg's rawvideo and Cinepak decoders
  (`docs/NOTICE.md`); `VideoDecoder` opens them through `openOwnVideo` with the decoders of the next sections. Cinepak and `DIB ` need
  configType `bmih`; every picture comes out with its own sample's time.
- `DIB ` (`BI_RGB`): 1, 2, 4 and 8 bits through the palette at the end of the format block (FFmpeg's rule; 1 bit without a palette
  is white and black), 15 and 16 bits as RGB 5:5:5 widened by repeating the top bits (`v << 3 | v >> 2`), 24 bits BGR, 32 bits BGRA (the fourth byte is the alpha plane, as in FFmpeg's `bgra`).
  Rows are padded to 4 bytes when the packet holds the padding (else packed); a picture whose `biHeight` is positive is bottom-up and
  is turned over. `BI_BITFIELDS` is refused.
- Raw YUV: the planar fourccs hand their planes out as they are (`YV12`, `YV16` and `YVU9` with the chroma planes swapped), the
  packed 4:2:2 ones and NV12/NV21 are split into planes, grey (`Y800`, `GREY`) comes out full range with neutral chroma. `cyuv` is
  bottom-up.
- Cinepak: V1 and V4 codebooks per strip kept from picture to picture, strips sharing codebooks within a picture unless the frame
  flags say otherwise, selective codebook and vector updates, pictures whose size is no multiple of 4, the 8-bit paletted form (the
  palette from the format block) and FFmpeg's checks for damaged and Sega FILM data.
- RGB pictures (raw RGB, palettes, Cinepak) are three planes G, B, R at 4:4:4 with `Color.matrix` 0 (identity) and full range, as an
  H.264 or HEVC stream with matrix coefficients 0 would be. `pictureToBGRA` copies them straight, `pictureToI420` and the scaler's
  I420 output go through BGRA and `bgraToI420`, `ffamp decode` writes the G, B, R planes (ffmpeg's `gbrp`). On the Mac they are stored
  as they are in a full-range 4:4:4 pixel buffer (G in the luma plane, B and R as the chroma pair) and the frame keeps matrix 0.

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
- Complex profile decodes with the simple IDCT those streams ask for (FFmpeg's C version, which its VC-1 decoder names, not the NEON
  one); its X8 intra pictures (`kelsy_300.wmv`, `sample-wmv9-complex-CP.wmv`) go through the X8 decoder of WMV8 (`src/intrax8.cpp`,
  quantiser 2 x PQUANT + HALFQP, offset PQUANT for the uniform quantiser, the sequence header's loop filter flag). Sprite streams
  (`WMVP`, `WVP2`) are refused.
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
  - `refusal`: set by `counts` when the stream turns out to use a mode the decoder refuses; `AudioDecoder::open` then fails with it.

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

## Windows Media Audio 9 Professional (`wmap`, `src/wmapro_decode.cpp`)

- Based on FFmpeg's decoder (`docs/NOTICE.md`) and checked against it: float output, 1 to 8 channels in the order of the channel mask
  (or the stated count when the mask is 0), any bits per sample from 1 to 32 (16, 20 and 24 tested), frames of 128 to 8,192 samples,
  up to 32 sub-frames per channel with their own layout per channel, M/S and multichannel transforms (default matrices up to 6
  channels, coded rotations for any group), per-band transform switches, scale factors resampled between sub-frame sizes, vector and
  run-level coefficients, frames with or without length fields, the bit reservoir across packets, dynamic range bytes (read, not
  applied, as in FFmpeg), start and end trims. Each container packet is cut into `block_align` pieces; FFmpeg's sequence-number check
  and its handling of lost pieces are kept.
- Channel order is the mask's bit order, as FFmpeg gives it; 6 channels with mask 0x3F are FL FR FC LFE BL BR, 0x60F FL FR FC LFE SL
  SR, 8 channels with 0x63F FL FR FC LFE BL BR SL SR, so the mixer and the WAV writer take them as they are. Probe names the layout
  from the mask (`quad`, `5.1(side)`, `7.1`), for 9 Lossless too.
- Start: FFmpeg drops the first decoded frame; FFamp decodes it and hides it (`skip` = one frame). A start trim in that frame does
  nothing (FFmpeg applies it to the dropped frame); later trims cut the frame they are in. No frame is added at the end (FFmpeg's
  decoder has no end flush).
- Frame counts: `counts` parses every packet once without the transforms, on all cores: frames are parsed independently of earlier
  frames, so the packets are split into runs, each run started one packet early by its own copy of the decoder. The count is what
  the straight decode gives (trims included).
- Jumps restart three packets early and at least two frames plus the largest packet's frame count early (the first packet after a
  restart loses the frame that started before it, and the first decoded frame lacks its overlap); no state marks are needed.
- On purpose unlike FFmpeg: a piece that would decode more than 16 million samples is cut off (a guard against crafted streams);
  an unknown scale-factor code is an error instead of a read before the table.

## Windows Media Audio 9 Lossless (`wmal`, `src/wmalossless_decode.cpp`)

- Based on FFmpeg's decoder (`docs/NOTICE.md`) and bit-exact with it: 16 and 24 bits (output as float, `x / 32768` and `(x << 8) /
  2^31`, so the integers come back exactly), 1 to 8 channels, raw PCM tiles, the Rice-style residues with their running mean, the
  cascaded LMS filters (16- and 32-bit, FFmpeg's C arithmetic), the multichannel LMS, the stereo decorrelation, the AC filter,
  quantiser steps and padding bits, frames with and without length fields, sub-frames, end trims, the bit reservoir and the final
  frames held in the reservoir at the end of the stream (FFmpeg's drain).
- Not decoded, as in FFmpeg: arithmetic-coded tiles (the frame is lost and the stream picks up again at a seekable tile); the
  inverse LPC filter of the `0x100` streams (its coefficients are read, the filter is not applied, so such tiles come out wrong in
  both); frames before the first seekable tile give no sound. No public sample uses these modes.
- The filters carry their state from one seekable tile to the next (about every 10 frames), so a restart a few packets back is not
  enough: `save` keeps the decoder state (the reservoir, the parse state and the filters in use, 42 KB for `luckynight.wma`) and
  `AudioDecoder` restores the nearest mark and decodes forward from it without output. A first jump far ahead therefore decodes
  everything up to it once (about 200 times real time); later jumps into runs already passed cost at most 20 seconds of decoding.
- Frame counts: one sequential parse at open (residues only, no filters).
- On purpose unlike FFmpeg: at the end of a cut file, a last frame that reads past the data takes the bytes that follow in the
  reservoir; FFmpeg's reader stops moving at the end and repeats bits, so that frame differs (FATE `Mega_Weird_Audio_Test_24bit`
  and `g2_24bit`).

## Windows Media Audio 9 Voice (`wmas`, `src/wmavoice_decode.cpp`)

- Based on FFmpeg's decoder (`docs/NOTICE.md`) and bit-exact with it when FFmpeg runs its C code (`-cpuflags 0`) built from the
  reference source: mono float output, 322 to 22,097 Hz (FFmpeg's limits), 10 or 16 LSPs coded per frame or once per superframe
  (residual), all 17 frame types (comfort noise, hardcoded codebook, pitch-adaptive window pulses, single and double excitation
  pulses, per-frame asymmetric and per-block Hamming pitch), the averaging projection postfilter (Kalman smoothing, the Wiener denoise
  filter with its tilt correction, gain control, the DC filter), superframes split across packets, sample counts below 480,
  statistics fields. Packets that carry several `block_align` units (AVI chunks, ASF media objects) are split as FFmpeg splits them.
- The postfilter's transforms (real FFT of 128, DCT-I and DST-I of 64) are FFmpeg's C transforms with its butterflies, PFA maps and
  tables (`src/wmavoice_dsp.cpp`), and every float expression keeps FFmpeg's order and types, so the compiler fuses the same
  multiply-adds. FFmpeg's arm64 build runs its NEON FFT for the 128-point transform, which rounds differently (see `docs/RESULTS.md`).
- Bits are read as FFmpeg's checked reader reads them (the position stops 8 bits past the end, bits behind the end come from the rest
  of the packet), so a superframe that runs past its data fails as in FFmpeg, with the decoder state FFmpeg is left with.
- Music frames (the first bit of a superframe clear: Windows Media Audio 9 Professional inside Voice) are not decoded by FFmpeg either.
  `counts` finds them, and `AudioDecoder::open` refuses such a stream with `this Windows Media Audio 9 Voice stream has music frames
  (Windows Media Audio 9 Professional inside Voice), which are not decoded` (`PacketDecoder::refusal`).
- Every superframe feeds the next (excitation, synthesis, postfilter memories, the noise generator's frame counter), so `save` keeps
  the whole decoder state (about 7 KB) and jumps go through the marks; one preroll packet puts each run's mark inside the run before,
  so a straight read never decodes anything twice.
- Frame counts: one parse of every packet at open without synthesis (about 4 % of the decode time; 16 ms for 600 s).

## The H.263-family core (`src/h263.h`, `src/h263.cpp`)

- What the macroblock decoders of this family share, based on FFmpeg's `MpegEncContext` code (files in `docs/NOTICE.md`): `Mpv` holds
  the macroblock geometry, the DC, AC and coded-block prediction arrays (kept across pictures, as FFmpeg keeps them), block indexes,
  H.263 motion vector prediction, the MPEG-4 AC prediction, the H.263 intra dequantiser, 16x16 half-pel motion compensation with edge
  emulation, the H.263 loop filter, and the H.263 and Microsoft MPEG-4 VLC and run-level tables (`src/msmpeg4_tables.cpp`).
- It reuses what VC-1 already has instead of copies: the bit reader and VLC tables (`src/bits.h`, `src/vlc.h`; `Vlc::lookup` decodes
  from a cached 64-bit window so the run-level loop reads code and sign with one load), `vc1::Bits`, the frame pool and picture
  buffers (`vc1::FramePool`, now shared, 64-pixel margins), the half-pel copy and average functions, edge emulation, the simple IDCT
  family and the Microsoft MPEG-4 DC, intra pattern and scan tables of `src/vc1_tables.cpp`.
- IDCT and half-pel averages follow the FFmpeg of each system, for every decoder on the core (H.263, Microsoft MPEG-4 v1 to v3, WMV7,
  WMV8): on arm64 FFmpeg's NEON simple IDCT, which computes rows without the C version's shortcut for rows that hold only a DC value
  (`vc1::neonSimpleIdctPut`, `neonSimpleIdctAdd`), and exact averages; elsewhere the C simple IDCT with that shortcut (`h263::cIdctPut`,
  `cIdctAdd`; FFmpeg's x86-64 SSE2 IDCT copies it) and, on x86-64, the approximate 8-wide no-rounding horizontal and vertical half-pel
  averages FFmpeg's x86 build uses (`h263::pixelOps()`). WMV8 uses its own IDCT (`wmv2IdctPut`, `wmv2IdctAdd` in `src/intrax8.cpp`).
  All five versions are bit-exact with ffmpeg on x86-64 and arm64; `tests/vc1_check.py` no longer accepts a PSNR floor for them.

## Microsoft MPEG-4 v1 to v3, WMV7 and WMV8 (`src/msmpeg4.cpp`, backends `msmpeg4v1`, `msmpeg4v2`, `msmpeg4v3`, `wmv1`, `wmv2`)

- One decoder for the five versions, based on FFmpeg's (`docs/NOTICE.md`), behind `SoftDecoder` (`openMsmpeg4`). The track's width
  and height set the macroblock grid; WMV8 reads its 4 bytes of codec data (bit rate, loop filter, mspel, adaptive transform types,
  X8 pictures, slices) from behind the BITMAPINFOHEADER; the others need no codec data.
- I and P pictures, every VLC set of each version, the three escape kinds, per-macroblock run-level tables, inter-intra DC prediction
  (WMV7 at low bit rates), flip-flop rounding, slices, WMV8 skip maps, quarter-pel mspel motion, 8x4 and 4x8 transforms, the loop filter
  and WMV8 X8 intra pictures (`src/intrax8.cpp`, the J-frames; VC-1 Complex profile uses the same code).
- Every sample gives one picture with its own time. A WMV8 P-picture in which every macroblock is skipped carries no picture: the one
  before is shown again with this sample's time (FFmpeg drops it). A picture whose macroblocks stop decoding part way keeps what was
  decoded and fills the rest from the picture before (grey at the start); FFmpeg conceals with its error resilience, so such pictures
  differ. A jump starts a fresh decoder at the key frame before; the prediction state FFmpeg keeps across pictures is rebuilt by every
  I-picture, so jumps give the straight decode.

## H.263 and H.263+ (`h263`, `src/h263_decode.cpp`, backend `h263`, Windows and Linux)

- FFamp's own decoder on the H.263 core, based on FFmpeg's (`docs/NOTICE.md`), behind `SoftDecoder` (`openH263`); `VideoDecoder`
  picks it for `h263` on Windows and Linux. The Mac keeps VideoToolbox for H.263.
- The picture size comes from each picture header (the five standard formats and H.263+ custom sizes, which may change between
  pictures). Baseline, GOB headers, long vectors, PB-frames (the B part is read and dropped, as FFmpeg does), and the H.263+ options
  FFmpeg decodes: unrestricted vectors, advanced prediction (4MV and OBMC), advanced intra coding, deblocking, slices, alternative
  inter VLC, modified quantisation, rounding type. H.263+ B-pictures (Annex O) are not decoded (`decode` returns `false`).
- Bit-exact with the FFmpeg of each system through the core's IDCT and averages (above): on x86-64 the C simple IDCT with its DC-row
  shortcut and FFmpeg's approximate averages (`ff_put_no_rnd_pixels8_x2_approx`, `_y2_approx`); arm64 builds take the NEON IDCT and
  exact averages, as FFmpeg there.
- In AVI, H.263 key frames are the I-pictures read from each chunk's picture header (FFmpeg's AVI writer marks every chunk).
- Macroblocks that do not decode are filled from the picture before (grey at the start); FFmpeg's error resilience differs there.

## Editing, lossless and uncompressed video (`src/own_video.h` and the decoders below, every system)

- Codecs FFmpeg decodes for MOV, AVI and MKV files from editors, capture tools and screen recorders, as FFamp's own `SoftDecoder`s
  behind `openOwnVideo` (`ownVideoName` gives FFmpeg's codec name, which probe reports). Desktop `VideoDecoder` and the Mac's soft path
  both use them. Each one is bit-exact with ffmpeg's planes (and alpha) on the test files of `tests/suites/62_videofix.sh`.
- `Planes` (`src/own_video.h`) holds a decoder's picture: 8 or 16-bit samples, any chroma size, an optional alpha plane (handed out
  at 8 bits, rounded); `rgb` labels G, B, R planes as matrix 0 full range.
- DNxHD and DNxHR (`src/dnxhd_decode.cpp`, `src/dnxhd_tables.cpp`, backend `dnxhd`, based on FFmpeg's): every CID FFmpeg knows, 8, 10
  and 12 bits, 4:2:2 and 4:4:4 (RGB as G, B, R; with the adaptive colour transform as YCbCr), interlaced two-field frames, MBAFF,
  DNxHR's variable sizes; the simple IDCT of each bit depth as FFmpeg runs it (8 bits: the C IDCT on x86-64, the NEON one on arm64).
  Rows are decoded on the pixel crew. 4:4:4 at 8 bits is refused, as in FFmpeg.
- QuickTime Animation (`src/qtrle_decode.cpp`, backend `qtrle`): 16, 24 and 32 bits per pixel (32 with alpha). Each frame updates
  the lines it names on the picture before, so a jump starts at a key frame. The palette depths (1, 2, 4, 8 bits and grey) are not
  decoded; `VideoDecoder::open` says so.
- PNG (`src/png_decode.cpp`, backend `png`, written from the PNG specification and RFC 1950/1951 with its own inflate): grey, grey and
  alpha, RGB, RGBA and palette pictures at 1 to 16 bits, transparency chunks, Adam7 interlacing; one file per sample (MOV `png `, AVI
  `MPNG`). 16-bit pictures stay 16-bit.
- HAP (`src/hap_decode.cpp`, backend `hap`, based on FFmpeg's): `Hap1` (DXT1), `Hap5` (DXT5 with alpha), `HapY` (scaled YCoCg DXT5),
  `HapM` (YCoCg with an RGTC1 alpha texture), `HapA` (RGTC1 alpha only, shown as grey); Snappy and uncompressed chunks, the decode
  instructions container. Snappy is FFamp's own (from the format description). The 4x4 blocks are cut to the picture size.
- Ut Video (`src/utvideo_decode.cpp`, backend `utvideo`, based on FFmpeg's): the 8-bit formats (4:2:0, 4:2:2, 4:4:4, RGB, RGBA, BT.601
  and BT.709), the 10-bit `UQ` formats, the `UM` pack modes; none, left, gradient and median prediction, slices, interlaced coding.
  Odd picture sizes in subsampled formats are refused, as in FFmpeg.
- FFV1 (`src/ffv1_decode.cpp`, backend `ffv1`, based on FFmpeg's): versions 0 to 4, Golomb-Rice and range coding with default and
  custom state tables, several quantisation tables and initial states, slices (decoded on the pixel crew), context carried across
  frames until the next key frame. YCbCr 4:4:4 to 4:1:0 at 8 to 16 bits with or without alpha, grey (with neutral chroma), RGB at 8 to
  16 bits with or without alpha (the reversible colour transform). Not decoded: the float, remap and Bayer modes of version 4.
  Slice CRCs are not checked and a damaged slice is not concealed from the picture before (FFmpeg does both).
- v210 and uncompressed video (`src/raw_video.cpp`, backends `v210` and `rawvideo`): v210 with FFmpeg's line strides (48-pixel or
  24-pixel alignment); planar and packed 4:2:0 and 4:2:2, NV12/NV21, grey, and BI_RGB / QuickTime RGB at 16, 24 and 32 bits.
- CineForm (`CFHD`) is not decoded yet.
- Matroska `V_UNCOMPRESSED` takes its fourcc from the track's UncompressedFourCC element, `V_FFV1` its record from CodecPrivate.

## G.711, ADPCM and sound in more containers (`src/wave_audio.cpp`, gaps round)

- One decoder, based on FFmpeg's (`docs/NOTICE.md`), bit-exact with it (16-bit output as `x / 32768`): A-law and mu-law, IMA ADPCM
  of WAV (2, 3, 4 and 5 bits), QuickTime IMA (`ima4`, which carries its predictor from block to block as FFmpeg does), Microsoft
  ADPCM (1 to 6 channels), Yamaha, IMA DK3 and DK4, G.726 (2 to 5 bits, mono) and G.722 (mono, 8 bits per code word).
- Each packet is cut into `block_align` blocks (34 bytes per channel for `ima4`) and every block is decoded as FFmpeg decodes it,
  a short last block included; FFmpeg's sample counts per block are kept, so `counts` is exact without decoding and readers ask
  `waveFrames` for the frames in a packet.
- Yamaha, G.726, G.722 and QuickTime IMA carry state across packets: `save` keeps it (a few hundred bytes) and jumps go through
  the marks. The others decode every block on its own.
- WAV and AIFF-C: any of the tags above, and `alaw`, `ulaw`, `ALAW`, `ULAW`, `ima4` in AIFF-C, in packets of whole blocks;
  WAVE_FORMAT_EXTENSIBLE follows its sub-format (the AC-3 and E-AC-3 GUIDs mean tag 0x2000). MP3, MP2, AC-3 and E-AC-3 in WAV are
  cut into frames, FLAC (0xF1AC) into frames with the STREAMINFO of its format block, Windows Media Audio tags go to their
  decoders. Microsoft ADPCM with more than two channels states its block size per channel in WAV; it is multiplied by the channel
  count, as FFmpeg's WAV reader does. 64-bit integer PCM is read.
- AVI: the same tags, one packet per chunk with times from the exact frame counts; AAC (0x00FF, the AudioSpecificConfig behind
  the WAVEFORMATEX) and FLAC (0xF1AC) one frame per chunk; a sound stream with tag 0 whose data starts with an AC-3 sync word
  (FFmpeg's E-AC-3 in AVI) is cut into AC-3 / E-AC-3 frames.
- Matroska `A_MS/ACM` and ASF: the same tags. A Matroska block's frames come from its size, so DiscardPadding is taken off the
  decoded length, as in FFmpeg.
- QuickTime: `alaw`, `ulaw`, `ima4`, `twos`/`sowt` 8-bit (signed) and the `ms` codecs. Sound counted in frames (sample size 1) is
  cut into packets as FFmpeg cuts it (one block when a block holds 160 frames or more, else 1024 frames' worth of blocks), and the
  packet in which the edit list ends is played whole, as FFmpeg plays it.
- CAF: `alaw`, `ulaw`, `ima4`, the `ms` codecs (block size and bits from the description), 8-bit PCM (signed), and FLAC without
  a `kuki` chunk (FFmpeg writes none): the STREAMINFO is built from the first frame header.
- AAC in WAV is not read: FFmpeg writes it without framing and cannot decode it either.
- TTA (`src/tta_decode.cpp`, Matroska `A_TTA1`): based on FFmpeg's decoder and bit-exact with it, 8, 16 and 24 bits, 1 to 16 channels,
  one frame per block; the last frame is cut to the header's length when its bits end there, as in FFmpeg. Encrypted streams (a
  password) are refused, as FFmpeg refuses them without one.

## Windows and Linux (`src/desktop`, `src/windows`, `src/linux`, `docs/PLAN_WINLINUX.md`)

- One desktop layer for both systems: `VideoDecoder`, `VideoEncoder`, `AacEncoder`, `Scaler`, the GIF reader and the capture session
  (test devices only so far). Frames live in memory: `Frame.native` is an `Image` (always planar, 8 or 16-bit samples with the decoder's
  `shift`; two-plane pictures from the GPU are split while they are copied in, rows spread over the pixel crew), recycled through a small
  pool; `withPixelBuffer` hands out its `Picture`. Every decoder is a `SoftDecoder` and every encoder a `SoftEncoder`, so the jump,
  read-ahead, concealment and field-pair logic is the Mac's soft path.
- `SoftDecoder::wanted(stamp)` is set by `VideoDecoder`: false for pictures it will not keep (before the asked frame after a jump, or
  everything in `run()`). Media Foundation and VA-API then skip the copy from the GPU and hand a picture with only its stamp and colour
  (no planes), which `store` counts and drops; software decoders ignore it.
- Platform hooks, picked at run time with the software libraries behind them: `openPlatformVideo` (Media Foundation H.264, HEVC and MPEG-1/2, through
  the GPU when the MFT takes a D3D11 device manager: backend `mediafoundation-gpu`, else `mediafoundation`), `openPlatformEncoder` (the
  GPU driver's encoder MFT, then Microsoft's software MFT; no B-frames), `openPlatformAudio` (Media Foundation AAC, which hands its sound
  out one frame late: `lead` is one frame; refused for a program config element (channel configuration 0: it gave silence for 2.1 and its own element order, unlabelled, for the rest) and whenever Media Foundation would give another rate
  or channel count than the configuration states, such as LC at 24 kHz or less and HE-AAC whose configuration does not signal SBR, which
  Media Foundation takes for double rate: those go to FFamp's own decoder; a stream change it announces is renegotiated, and one that
  changes the format ends that decoder's sound), `openPlatformAac` (44.1 and 48 kHz at 96, 128, 160 or 192 kbit/s, no priming), and
  `openStillDecoder` (WIC on Windows, libjpeg-turbo on Linux). On Linux `openPlatformVideo` is VA-API (below); the other hooks return
  nothing there yet. Media Foundation pictures are cut to the output type's minimum display aperture, so left and top cropping is kept.
- The system decoder comes first and the software decoder stands behind it, on every system: Media Foundation for H.264, HEVC and
  MPEG-2 on Windows, VA-API for H.264, HEVC, VP9 and AV1 on Linux, FFamp's own H.264 decoder, libde265, libmpeg2, libvpx and dav1d
  behind them. The software decoder takes a stream the system decoder refuses at open (Windows N without Media Foundation, H.264
  above 8 bits or other than 4:2:0, pictures under 64 pixels, no driver), and also later: when the system decoder's `decode` fails, or
  a frame does not come out of it even after the restarts at earlier key frames, `VideoDecoder` opens the software decoder once,
  starts again at a key frame and keeps it for the rest of the stream (`backend()` then names it). `FFAMP_OWN_H264` puts FFamp's own
  H.264 decoder first (Windows and Linux; for tests and speed comparisons); `FFAMP_FAIL_SYSTEM_VIDEO=N` makes the system video
  decoder fail from its Nth sample, to test that hand-over.
- Media Foundation and VA-API only get the profiles they decode: H.264 8-bit 4:2:0, HEVC Main and Main 10 4:2:0 and MPEG-1/2 4:2:0
  (the chroma format of the first sample's sequence extension, `mpegChroma`); Media Foundation only HEVC no larger than level 6.2
  allows (35,651,584 samples, 16,888 a side; the profile, chroma format and bit depths come from `hvcC`). Media Foundation accepts
  every HEVC and MPEG-2 stream and then hangs (GPU), waits forever in its drain or gives no pictures (software) on HEVC 4:2:2, 4:4:4,
  monochrome, 12-bit and larger pictures and on MPEG-2 4:2:2, so those go to libde265 and libmpeg2 from the start.
- `encoders()` names the encoders `VideoEncoder` and `AacEncoder` really use, under FFmpeg's names for the same job: `h264_mf`,
  `hevc_mf` and `aac_mf` on Windows (x264, x265 and vo-aacenc behind them where Media Foundation cannot), `libx264` and `libx265` on
  Linux, and `aac` for vo-aacenc on both.
- Media Foundation on the GPU: each picture's texture is copied into one of three staging textures of FFamp's own and mapped two
  pictures later, so the copy never waits for the GPU (pictures leave the decoder up to two samples later; the drain hands out the
  rest, a reset drops them). One D3D11 device and DXGI device manager serve every Media Foundation decoder of a process (made again
  after a device removal). Microsoft's H.264 and MPEG-2 decoders are created by their class IDs, without the MFT enumeration. Their
  DLL (`msmpeg2vdec.dll`) calls `SetThreadAffinityMask` when a stream begins, and on the Windows test PC (with WSL 2 running) that
  call waited 0 to 1.4 s at the first stream of every process, also without the GPU; FFamp points that DLL's import of the function at one that leaves
  the affinity alone (`skipAffinityCalls`), and the first stream starts in about 1 ms.
- VA-API on Linux (`src/linux/vaapi*.cpp`, backend `vaapi`): libva and libva-drm are loaded with `dlopen` at the first video open, the
  first render node that initialises is used (Mesa's d3d12 driver under WSL, picked through `GALLIUM_DRIVER`), and any failure falls
  back to the software decoders; `FFAMP_NO_VAAPI` turns it off. The libva 2.22.0 headers come from `build_linux.sh` (`ABSENT_VAAPI`);
  nothing is linked. H.264 (8-bit 4:2:0, progressive, MBAFF and field pairs woven into one frame), HEVC Main and Main 10, VP9 profiles
  0 and 2 and AV1 Main (8 and 10-bit) are parsed by FFamp and sent as VA-API picture and slice buffers; pictures are copied out as
  NV12 or P010 after `vaSyncSurface2` (2 s at most), and a failed submit or copy-out stops the GPU path for that decoder, whose stream
  then goes to the software decoder (above). Under the
  d3d12 driver four cases stay on the software decoders (H.264 on FFamp's own decoder) because the driver gets them wrong: H.264 with non-flat scaling matrices
  (it ignores them), 10-bit VP9 and AV1 whose height % 16 is 2 to 9 (wrong chroma and inter pictures), VP9 and AV1 with an odd width
  or height (the driver crashes the process in `vaEndPicture`; `d3d12Wrong`), and AV1 film grain. NVIDIA's
  driver has no VA-API; see `docs/PLAN_WINLINUX.md` for what NVDEC would take.
- Software paths: FFamp's own H.264 decoder (below), libde265 (monochrome HEVC comes out as 4:2:0 with neutral chroma, as dav1d's
  monochrome AV1), FFamp's own AAC decoder (`src/aac_*.cpp`: LC, HE-AAC, HE-AAC v2) and
  AC-3/E-AC-3 decoder, opencore-amr (AMR-NB, AMR-WB), mpg123 (MP1, MP2, MP3 on every desktop system: Media Foundation's MP3 decoder
  places its sound 624 frames later than FFmpeg's), vo-aacenc (priming 1600), x264, x265, libvpx, dav1d, SVT-AV1, liboapv, xvidcore,
  libopus, libvorbis, LAME, libmpeg2 (MPEG-1/2 when no system decoder takes the stream: always on Linux, on Windows for 4:2:2 and
  without Media Foundation), libjpeg-turbo (Linux). libvpx also gives VP9 4:4:0 (profile 1).
  vo-aacenc swaps any bitrate above 160 kbit/s per channel (or 6 bits per sample per channel, or below 4 kbit/s per channel) for its
  default of about 64 kbit/s per channel, so `openVoAac` clamps the asked bitrate into that range: mono at 192 to 320 kbit/s is
  encoded at 160 kbit/s. AudioToolbox on the Mac gets the highest applicable bitrate at or below the asked one (mono stops at 256).
  `FFAMP_NO_MEDIA_FOUNDATION`, `FFAMP_NO_SYSTEM_AUDIO`, `FFAMP_NO_SYSTEM_ENCODER` and `FFAMP_NO_GPU_DECODE` force the
  software paths for testing; `VideoDecoder::open(..., software = true)` skips the system decoder.
- `bgraToI420` (every BGRA and RGBA frame handed to `VideoEncoder::encodeBGRA` off the Mac, and the Mac's software path) takes the
  chroma of each 2x2 block as the average of its four pixels; a 1-pixel coloured line keeps half its colour, as swscale's
  `rgb0` to `yuv420p` gives it (within 16 levels on the fixes suite's test frame, mean under 2).
- Before draining, the decoder feeds up to 16 more samples until the asked frame arrives, so decoders with an output delay (Media
  Foundation after a flush, the H.264 decoder's frame threads) need no drain inside a stream. `SoftDecoder::drainKeepsState` is false for
  decoders whose drain ends the stream (the H.264 decoder, Media Foundation, libmpeg2): after such a drain the next jump starts again at a key frame.
  `SoftDecoder::weavesFields` is true for Media Foundation and the H.264 decoder, which return one frame for an H.264 field pair. When a
  picture never arrives after a jump (a recovery point that is not an IDR, such as x264's intra refresh, whose pictures the H.264 decoder
  holds back until the refresh is complete, as FFmpeg does), the decoder starts again up to three key frames earlier before concealing.
  A `Picture` may carry `keep`, the decoder's own frame: the `Image` then points at the decoder's planes instead of copying them.
- Pictures coded as RGB (matrix 0: libx264rgb H.264, VP9 and AV1 `gbrp`, and the RGB modes of the decoders in the next section) carry
  their G, B and R planes at full size in `plane[0..2]`. BGRA, RGBA and RGB24 output reorder them (exact at 8 bits), I420 output
  (`pictureToI420`, the scaled frame reader, the encoders) converts them to 8-bit BT.709 limited range with `bgraToI420`, and a
  picture's matrix 0 wins over the container's colour labels (`rgbPlanes`). `decode` writes the planes as ffmpeg's `gbrp`.
- A frame the stream leaves without colour labels is labelled as VideoToolbox labels it: BT.601 with SMPTE C primaries below 720
  lines, BT.709 from 720 lines. Media Foundation is not used for pictures under 64 pixels wide or high (its decoder returns nothing)
  nor for encoding frames under 320 x 240 (the GPU encoders undershoot the bitrate and smear such frames); x264 and the H.264 decoder take them.
  H.264 with more than 8 bits or other than 4:2:0 goes straight to the H.264 decoder (Media Foundation and the VA-API path do not decode it).
- H.264 (`src/h264`): FFmpeg 9.0.2's H.264 decoder ported in plain C (the library's only C, kept close to FFmpeg's files so its assembly
  and later fixes carry over) over a small libavutil layer (`ffbase.h`, `ffbase.c`), with FFmpeg's frame threads and slice threads
  (`h264_ffamp.c`, API in `h264_ffamp.h`), FFmpeg's x86-64 assembly (NASM, `x86/`, built when CMake finds NASM; `ABSENT_H264_ASM=OFF` turns
  it off) and AArch64 NEON (`aarch64/`). `h264_soft.cpp` is the `SoftDecoder` (backend `h264`). Every profile FFmpeg decodes (Baseline to
  High 4:4:4 with lossless, 8 to 14 bits, PAFF and MBAFF, monochrome, RGB as G, B and R planes labelled matrix 0), FFmpeg's error
  concealment, H.274 film grain applied as FFmpeg applies it (model 0, 8-bit 4:2:0), exact cropping as the ffmpeg command line applies it. Pictures that are not recovered (before the first IDR or
  recovery point) are held back, as in FFmpeg. Test switches: `FFAMP_H264_THREADS` (0 is FFmpeg's choice, the CPU count plus one up
  to 16), `FFAMP_H264_SLICES` (slice threads instead of frame threads), `FFAMP_H264_CPU` (a mask on the CPU features; 0 runs the plain
  C), `FFAMP_H264_FILL` (fills new frame buffers with a byte, to find output that depends on unwritten memory). `ffamp h264 <file>`
  decodes a file's H.264 track with it on every system, the Mac included, and writes the raw planes.
- The H.264 decoder starts with the reorder delay FFmpeg's stream probe finds (`ffamp_h264_guess_delay`): when a sequence parameter set
  of the configuration has no VUI reorder count, a throwaway one-thread decoder runs over the first packets (7 to 20 shown pictures,
  250 packets at most) and its reorder delay is given to the real one, so pictures shown before the first key picture in display
  order are not dropped as out of order. `VideoDecoder` and `ffamp h264` both do this; with a reorder count in the stream nothing is
  decoded twice.
- Every libavutil and libavcodec name the port defines carries the `ffamp_` prefix (`src/h264/ffamp_names.h`, force-included in each
  C file; the x86 assembly's `private_prefix` is `ffamp_ff`, the AArch64 assembly's `EXTERN_ASM` is `ffamp_`), so FFamp links into one
  library with FFmpeg without standing in for its functions. A name added to the port goes into that header; the H.264 suite fails
  when the library defines an `av_`, `avpriv_` or `ff_` symbol.
- FFmpeg's FATE h264 conformance tests (`tests/h264_fate.txt`: 185 bitstreams of the ITU-T H.264.1 set in seven groups) run when
  `H264_CONFORMANCE` names the `h264-conformance` folder of the FATE suite (or `FATE_SAMPLES` the suite); each file is compared frame
  by frame with `-f framemd5` against ffmpeg with one thread, frame threads and slice threads. The bitstreams are not in the repository.
  On Windows every file also goes through `VideoDecoder` in both orders (`h264_check.py ... --decode`); with Media Foundation first,
  frames 10 and 28 of `FRExt/HPCAMAPALQ_BRCM_B` may differ (Microsoft's decoder, RESULTS), any other difference fails.
- FLAC (written from RFC 9639) and ALAC (based on FFmpeg's) are FFamp's own decoders on Windows and Linux, bit-exact with ffmpeg.
- AC-3 and E-AC-3 are FFamp's own decoder on Windows and Linux (`src/ac3_decode.cpp`, based on FFmpeg's `ac3`/`eac3` float decoders,
  NOTICE), 135 dB or better against ffmpeg with its length and timing to the sample (no lead). It follows FFmpeg 9.0, which
  dequantizes to whole numbers (each mantissa shifted right by its exponent) and uncouples in fixed point, not the float coefficients
  of the 8.0 sources. Every acmode with and without LFE, 32, 44.1 and 48 kHz and AC-3's halved rates, coupling, rematrixing, block
  switching, dither from FFmpeg's generator (seed 0, continuing across packets), dynamic range at FFmpeg's default scale, and for
  E-AC-3 AHT, SPX and dependent substreams with FFmpeg's channel map. A block with enhanced coupling (FFmpeg refuses it) repeats the
  last good block as FFmpeg's concealment does; E-AC-3 at a reduced sample rate (FFmpeg decodes none of it) is refused at open.
  The dither generator, the delay upmix FFmpeg does once per stream (on the first block whose channels switch block size differently)
  and the E-AC-3 band structures are kept in the 20-second marks, so random reads equal the straight decode. Long transforms of up
  to four channels run together in one SIMD pass with `InverseMdct`'s arithmetic, bit for bit. `tests/ac3_make.py` writes streams
  with the syntax ffmpeg's encoders never use (dual mono, 2/2 with LFE, block switching, AHT, SPX, one to three blocks per frame,
  dependent substreams, enhanced coupling) for the checks against ffmpeg.
- ProRes and DV (based on FFmpeg's) are FFamp's own video decoders on Windows and Linux (backends `prores` and `dv`), bit-exact with
  ffmpeg: ProRes 422 as 10-bit, 4444 and XQ as 12-bit with the alpha plane handed out at 8 bits as swscale rounds it ((a + 8) >> 4);
  DV in the size and chroma layout of the profile its frames name (4:1:1, 4:2:0, 4:2:2; DVCPRO HD 1080 is 1280 or 1440 wide, as in
  FFmpeg). 4:1:1 pictures convert to BGRA with the chroma sample centred on its four pixels, as swscale does.
- MPEG-1/2 on Linux, and on Windows for 4:2:2 streams and Windows without Media Foundation: libmpeg2 (backend `libmpeg2`,
  `openMpeg2`), used whenever `openPlatformVideo` gives nothing. `build_windows.sh` builds it with MSVC from its C code only (its x86
  code is GCC inline assembly), so its IDCT on Windows is the C one; 4:2:2 MPEG-2 comes out as 4:2:2, 51 dB or better against ffmpeg. Each sample's time
  goes in as libmpeg2's picture tag and comes back on the picture it shows, so B-frame order needs no help; field pictures are woven
  in libmpeg2's own frame, and a sequence header with a new size ends the old pictures and starts again. A slice ends only at the
  next start code, so pictures come out one sample late; the drain (a sequence end code) ends the stream. B-pictures that lead an
  open GOP with no earlier reference (a file cut there, the first pictures after a jump) are handed out as the key picture they lead
  to, where FFmpeg drops them. Colour comes from the sequence display extension; the range is always limited. libmpeg2's x86 IDCT
  (SSE2) rounds differently from its C IDCT and from FFmpeg's: 60 to 65 dB against ffmpeg. The Windows build is libmpeg2's C code
  only (MSVC takes none of its inline assembly).
- Motion JPEG and JPEG pictures on Linux: libjpeg-turbo (backend `libjpeg-turbo`, `openStillDecoder`), as WIC on Windows: the
  picture's own YCbCr planes (4:2:0, 4:2:2, 4:4:4, 4:1:1; greyscale with neutral 4:2:0 chroma) from `jpeg_read_raw_data`, full
  range, BT.601; RGB and other layouts through BGRX into limited BT.709 4:4:4, as WIC's fallback. libjpeg-turbo fills in the standard
  Huffman tables when a frame has none (AVI Motion JPEG). Two-field Motion JPEG packets are not put together (WIC does not either).
- The formats suite's MPEG-1/2 and Motion JPEG checksums are VideoToolbox's on the Mac and `pictures_linux.md5` on Linux (x86-64,
  libmpeg2's SIMD path); Windows skips them (Media Foundation and WIC), and every system judges the same files against ffmpeg within
  the suite's floor.
- Libraries: `third_party/build_linux.sh` (gcc, static, PIC, assembly with nasm) and `third_party/build_windows.sh` (Git Bash in a
  vcvars64 shell; the libraries build with MSVC, xvidcore with clang-cl, GNU make built from source for x264 and libvpx; NASM from the
  portable zip of nasm.us on PATH, so x264, x265, dav1d, SVT-AV1, libvpx (x86-64 Windows target) and FFamp's own H.264 decoder build their assembly;
  NASM is a build tool only and never ships). FFamp builds with gcc or clang on Linux and clang-cl on Windows (`libffamp.lib`, so
  the CLI's import library cannot overwrite it; clang's builtins library is linked for 128-bit division). Paths given to FFamp are UTF-8
  on every system (`openFile`, `removeFile`, `renameFile`, `fileBytes` in `src/portable.h`); on Windows the `ffamp` command rebuilds
  its arguments as UTF-8 from the wide command line, as FFmpeg's programs do, and its commands open files only through `openFile`.
  On Windows `openFile` raises the C runtime's limit of open stdio streams from 512 to 8,192 (the limit of the low-level handles
  FFmpeg's file protocol uses), since every open reader holds one: a process can keep more than 509 files open.

## DTS, TrueHD and WavPack (`src/dts_decode.cpp`, `src/truehd_decode.cpp`, `src/wavpack_decode.cpp`, every system)

- FFamp's own decoders, based on FFmpeg's (`docs/NOTICE.md`), on every system (no system decoder has them).
- DTS: the core (every audio mode, LFE, joint intensity, ADPCM prediction, high-frequency VQ, block and Huffman codes, sum/difference)
  with the XCH and XXCH channel extensions inside the core substream and an embedded XXCH downmix undone (ported, not tested: ffmpeg
  cannot write them), through FFmpeg's float synthesis filter (`InverseMdct` for its 32-point transform), output in FFmpeg's channel
  order (5.1 is `5.1(side)`). 140 dB or better against ffmpeg for FFmpeg's own stereo and 5.1 streams, same length and timing. Not decoded: the extension substream (XLL lossless, LBR, XBR, X96 in either substream), so DTS-HD
  Master Audio plays its lossy core (FFmpeg plays the lossless stream) and a stream without a core (DTS Express) does not open.
  Packets that are not big-endian 16-bit are converted as FFmpeg converts them.
- TrueHD and MLP: bit-exact with ffmpeg (24-bit as `x * 256 / 2^31`, 16-bit MLP as `x / 32768`), up to three substreams (Atmos'
  fourth is ignored, as in FFmpeg), the noise generators, rematrixing, FIR and IIR filters, the end-of-stream shortening (found in the
  last packet when the decoder opens). A jump restarts 128 access units early, so the decoder passes a major sync.
- WavPack: bit-exact with ffmpeg for 16, 24, 32-bit and float streams, mono, stereo and multi-block streams with their channel mask
  (hybrid, lossy streams follow FFmpeg's code but ffmpeg cannot write one to test); every frame decodes on its own. DSD WavPack is
  refused.

