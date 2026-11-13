# File tools without FFmpeg

`include/absent/tools.h` and the `amedia` commands in `tools/cmd_tools.cpp` replace what the desktop app starts `ffmpeg` for in
`DeskCompress.kt`, `DeskTools.kt`, `DeskReverse.kt`, `DeskGifVideo.kt` and `DeskVideoDownloader.kt`. No other program is started.

## Shared behaviour

- **Times** are counted from the start of the file: the earliest shown sample of any track is zero (what `ffmpeg -ss` and ffprobe's
  format `start_time` do). The commands take `--start` / `--end` in seconds with decimals, or `--start-us` / `--end-us`.
- **Progress**: every command prints `progress 0.42` lines on standard output (two decimals, one line per changed value, flushed), then
  `progress 1.00` when the file is complete. The library functions take `Progress`, a function that gets the fraction and returns
  `false` to stop.
- **Stopping**: SIGINT or SIGTERM stops at the next frame or packet group. Data is written to `<output>.part` and only becomes
  `<output>` when the file is complete, so a stopped or failed run leaves no output file, no `.part` file and no scratch file. The
  exit code is 130 and the message is `amedia: cancelled`. Other failures exit with 1 and one line on standard error.
- **Output files start at zero** and keep picture and sound in step: a track that starts later than the other gets a leading empty
  edit in MP4/MOV, later block times in Matroska.
- A result line (for `reverse`, `gif-video`, `gif-info`, and `output <path>` when the command chose the extension) is printed on
  standard output next to the progress lines.

## Commands

| Command | What it does |
|---|---|
| `compress <in> <out> [--start s --end s] [--size WxH \| --fit WxH] [--fps N \| N/D] [--codec h264\|hevc\|av1\|vp9] [--bitrate bits] [--audio copy\|aac\|none] [--audio-bitrate bits]` | decode, cut, scale, change the frame rate, encode |
| `trim <in> <out> --start s --end s` | stream copy between two times |
| `extract-audio <in> <out> [--aac \| --mp3] [--bitrate bits]` | copy the first sound track, or re-encode it |
| `reverse <in> <out.mp4> [--start s --end s] [--bitrate bits] [--audio-bitrate bits] [--memory megabytes]` | picture and sound backwards into H.264 + AAC |
| `gif-video <in.gif> <colour.mp4> [<matte.mp4>]` | GIF frames into a colour video and, when something is transparent, a matte video |
| `gif-info <in.gif> [--frames out.bgra]` | size, delays, transparency; optionally every drawn frame as premultiplied BGRA |
| `merge <video file> <audio file> <out>` | first video track of one file + first sound track of the other, by stream copy |
| `strip-audio <in> <out>` | every video track, by stream copy |
| `rewrap <in> <out>` | every video and sound track into another container, by stream copy |

## Compress (`absent::compress`)

What the app's command does and what ours does:

| | App's FFmpeg command | absent-media |
|---|---|---|
| Cut | `-ss start` before the input (exact: frames before it are decoded and dropped), `-t end-start` | the same frames: first frame at or after the start, last frame before start + length |
| A span with no frame in it | the app looks for the frame shown before the start and seeks there | the same: that one frame is shown |
| Size | `scale=W:H` (stretches to exactly W x H, bicubic) | `--size WxH` stretches to exactly W x H; `--fit WxH` keeps the aspect inside the box; one side 0 derives it. Sides are made even. Scaling is done by VideoToolbox on the GPU, averaging when shrinking |
| Frame rate | `fps=N`: constant rate, frames dropped or repeated by timestamp, time rounded to the nearest slot, halves up | the same rule in whole numbers; the tests find the same source frame in every slot as in FFmpeg's file. Without `--fps` the source frame times are kept |
| First slot | the first output frame may sit at slot 1 with an edit list in front | the first frame is shown from slot 0, so picture and sound both start at zero |
| Encoder | `h264_videotoolbox -realtime 0 -profile:v high -b:v B -g fps` | VideoToolbox H.264 High, average bitrate B, key frame every `fps` frames, B-frames; also HEVC, AV1 (SVT-AV1) and VP9 (libvpx) |
| Colour | output labelled BT.709, limited range, pixels not converted | a source without colour labels is treated the same way (labelled BT.709, not converted). A source labelled as something else (BT.601, BT.2020, HLG, PQ, full range, 10-bit) is converted to 8-bit BT.709 limited by VideoToolbox, so the label is true |
| Rotation | `-noautorotate`, rotation flag carried over | the same |
| Sound, copy | packets from the one at or before the start | the same packets plus two run-in packets for AAC (four for Opus and MP3), and an edit list that starts and ends the sound on the exact sample |
| Sound, AAC | `aac_at -b:a K` from the packet at or before the start | AudioToolbox AAC at K, cut to the exact sample at both ends. More than two channels are mixed to stereo (FFmpeg keeps 5.1); a rate AAC does not take is resampled to the next one that it does |
| No sound track | `0:a:0?` is optional | the same, the file has no sound |
| Header | `+faststart` | the movie header is always in front |
| Progress | `out_time_us` | output time over total time |

Measured against FFmpeg's file for the same input (tests): same size, same frame count, same frame times to the millisecond, the same
source frame in each slot, file size within 30 %, picture quality against the source not below FFmpeg's.

Not done: more than two channels in re-encoded sound; a choice of scaling filter.

## Trim (`absent::trimCopy`)

`ffmpeg -ss start -i in -t length -map 0:v:0 -map 0:a:0? -c copy -avoid_negative_ts make_zero -movflags +faststart`

- The same first packet: the key frame at or before the start. Every packet is copied unchanged.
- **End**: ours shows frames up to `end` exactly. Packets after it that earlier frames need as references are kept and hidden by the
  edit list (MP4/MOV). FFmpeg cuts by decode time and keeps two or three more frames. In Matroska nothing can be hidden, so up to
  two frames past the end are shown.
- **Sound**: FFmpeg starts with the packet at or before the key frame, so its sound starts up to one packet before the picture and
  the picture gets an offset (0.083 s in the test file). Ours starts the sound on the exact sample of the key frame and ends it on the
  exact sample of the end, by edit list (MP4), CodecDelay and DiscardPadding (Matroska) or pre-skip and end granule (Ogg); the
  packets themselves are whole and unchanged, with two run-in packets for AAC, four for Opus and MP3, one for Vorbis.
- Pictures that follow an open-GOP key frame in the file but are shown before it cannot be decoded after the cut; FFmpeg copies them,
  ours leaves them out.
- A file with sound only can be trimmed (the app's command fails on it). `--end` at or before `--start` means "to the end".
- Every one of our video and sound packets is among FFmpeg's for the same call (tested).

## Extract Audio (`absent::extractAudio`)

- **Copy**: every packet unchanged, with the source's own priming and end trim. AAC and ALAC go into `.m4a`; `audioCopyExtension`
  names the file type for other codecs (`.mp3` gets the MPEG frames as they are, `.opus` an Ogg Opus file, anything else `.mka`). An
  output name without an extension gets the right one and the command prints `output <path>`. A codec the chosen container cannot
  hold is refused and the message names the extension to use. FFmpeg's copy of AAC gives the same packets (tested).
- **AAC**: AudioToolbox, the encoder behind `aac_at`, at the asked bitrate, into M4A.
- **MP3**: LAME, constant bitrate, at the source's sample rate (as `libmp3lame` does).
- Sources with more than two channels are mixed to stereo for both encoders. FFmpeg does that for MP3 and keeps 5.1 for AAC.
- The LAME information frame is not carried over when MP3 frames are copied into an `.mp3` file.

## Reverse (`absent::reverse`)

The app decodes parts of at most 2 seconds, reverses each with the `reverse` and `areverse` filters into MPEG-4 part 2 + PCM files,
joins them with the concat demuxer and encodes the sound to AAC. The result is MPEG-4 part 2 video in an MP4 file.

- Ours writes **H.264 + AAC** in one pass. The picture bitrate is 1.5 times the source's (2 times when the source is not H.264),
  between 1 and 80 Mbit/s, or `--bitrate`. Sound is 128 kbit/s (`--audio-bitrate`), stereo at most.
- **Frames**: every source frame shown in `[start, end)`; without a range, all of them. Frame N of the result is source frame
  (last - N), and each frame is shown exactly as long as in the source, so variable frame rates stay correct. The length is exact.
- **Memory**: one group of pictures is decoded at a time, the newest group first, and held as GPU frames while it is written
  backwards. A group that would need more than the budget (512 MB by default, `--memory`) is first written to a scratch file next to
  the output (`<output>.spill.mp4`: H.264, every frame a key frame, about 2 bits per pixel) and read back from the end. The result
  line reports how many groups went that way (`spilled N`). The scratch file is removed in every case.
- **Sound**: the same span, sample-exact, reversed. Where the sound ends before the picture, the result starts with silence.
- Odd sizes are stretched to the next even size by one pixel (the app pads with a black line). Rotation is carried over.
- Colour is handled as in Compress.

Against the app's FFmpeg steps on the test clip: same frame count, length and frame times; pictures agree at 36 dB or better.

## GIF to video (`absent::gifToVideo`, `absent::gifRead`)

- Frames are drawn by ImageIO (`CGImageSourceCreateImageAtIndex` gives the whole canvas with disposal applied) into a premultiplied
  BGRA canvas. ImageIO returns a frame that has no transparent colour as fully opaque, with black where nothing was ever drawn or
  where "restore to background" cleared the canvas. The app's own decoder leaves those places transparent, so the frame boxes,
  transparency flags and disposal methods are read from the file (no pixel data) and those places are made transparent again.
- On the five test files (full frames, part frames, the three disposal methods, a transparent colour, interlaced frames, local
  colour tables, a first frame smaller than the canvas) every frame equals an independent drawing of the GIF by the app's rules to the
  byte, colour and alpha. FFmpeg's GIF decoder gives the same pixels on the two files it is compared on.
- **Limits** as in the app: 2048 x 2048 pixels, 3000 frames, 600 million pixels over all frames, 10 minutes. A GIF with one frame is
  refused (the app keeps it as a picture).
- **Delays**: the app's rule `gifDelayMs`: 0 and 1 hundredths become 100 ms. The app's FFmpeg call (`-min_delay 2 -default_delay 10`)
  turns 0 into 100 ms but, in FFmpeg 9, keeps 1 hundredth as 10 ms, so its file is shorter than the length the app declares for it.
  Ours always matches the declared length. The last frame keeps its delay.
- **Size**: `gifVideoSize`, the app's `gifEncodeSize` with the desktop limit of 8192: both sides rounded to even (odd sides grow by
  one). Pixels are picked nearest-neighbour from the pixel centre, the mapping FFmpeg's `scale=flags=neighbor` uses.
- **Colour file**: premultiplied colour (transparent pixels are black). **Matte file**: alpha as grey. It is written only when at
  least one pixel of one frame is not opaque, as the app decides it; give the path and look whether the file exists, or read
  `transparent 1` in the result line.
- **Video**: H.264 from VideoToolbox at up to 20 Mbit/s (what the app passes when `libx264` is missing), BT.709 limited range and
  labelled so; time scale 100 (GIF's own clock), one frame per GIF frame. FFmpeg's `libx264` file is BT.601 and unlabelled.
- `fps` in the result line is the app's `gifFrameRate` (frames over length, rounded, 1 to 60).

A cut-off GIF gives the frames ImageIO can still draw, like the app's decoder.

## Downloader (`absent::merge`, `absent::stripAudio`, `absent::rewrap`)

See `docs/YTDLP.md` for the calls. All three copy packets unchanged, from any reader into the writer the output name picks.

- `merge`: when the output name has no extension, `mergeExtension` picks it: `.webm` for VP8/VP9 with Opus or Vorbis, `.mp4` for
  H.264, HEVC, AV1, VP9 or MPEG-4 video with AAC, ALAC, Opus or FLAC, otherwise `.mkv`. With an extension the caller decides and a
  codec the container cannot hold is refused. Each input starts at its own zero, as in FFmpeg's merge.
- `rewrap`: turns a transport stream download (also one saved under an `.mp4` name: readers go by content) into MP4. The gap
  between picture and sound is kept; MP4 stores it in milliseconds.
- Tested against FFmpeg's own merge and copy: the same packets.

## Changes to shared code

- `TrackSpec::delay` (track time scale): time before the track starts. `Mp4Writer` writes it as a leading empty edit and counts it
  in the durations; `MkvWriter` adds it to the block times.
- `VideoDecoder::pts(index)`: the time stamp of a shown frame without decoding it.
- `Mp3File` (LAME) now keeps the input sample rate instead of letting LAME lower it at low bitrates.
