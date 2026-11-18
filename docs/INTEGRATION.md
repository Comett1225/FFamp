# How AbsentMotion would call this (proposal)

Nothing in this file exists in the app yet. It describes the one interface the desktop code would use instead of starting `ffmpeg` and
`ffprobe` and instead of linking libav.

## Two seams

**1. The native preview decoder.** The app's native library already hides its decoder behind five JNI functions (open, info, frame,
frame for a layer, close). Their implementation is the only file that includes libav headers. It is rewritten on top of
`absent::Mp4Reader` and `absent::VideoDecoder`; the JNI names and the Kotlin side do not change. On macOS the decoder already hands out
the same kind of pixel buffer the Metal path wraps today, so the zero-copy path stays.

**2. One Kotlin object for everything else.** Every place that builds an ffmpeg command line calls this instead.

```kotlin
package app.absentmotion.media

object MediaEngine {
    fun probe(path: String): MediaInfo?

    fun openVideo(path: String): Long
    fun videoFrame(video: Long, frame: Int, into: ByteBuffer, format: PixelFormat, maxSide: Int): Boolean
    fun videoFrameAt(video: Long, timeUs: Long, into: ByteBuffer, format: PixelFormat, maxSide: Int): Boolean
    fun closeVideo(video: Long)

    fun openAudio(path: String, sampleRate: Int, channels: Int): Long
    fun readAudio(audio: Long, fromUs: Long, into: FloatBuffer): Int
    fun closeAudio(audio: Long)

    fun startExport(settings: ExportTarget): Long
    fun exportVideoFrame(export: Long, pixels: ByteBuffer, timeUs: Long): Boolean
    fun exportVideoSurface(export: Long, surface: Long, timeUs: Long): Boolean
    fun exportAudio(export: Long, pcm: ShortBuffer, frames: Int): Boolean
    fun finishExport(export: Long): String?
    fun cancelExport(export: Long)

    fun copy(job: CopyJob, progress: (Float) -> Boolean): String?
    fun compress(job: CompressJob, progress: (Float) -> Boolean): String?

    fun encoders(): List<EncoderInfo>
    fun cameras(): List<CaptureDevice>
    fun openCamera(device: CaptureDevice, mode: CaptureMode?, record: ExportTarget?): Long
    fun cameraFrame(camera: Long, into: ByteBuffer): Boolean
    fun closeCamera(camera: Long)
}

enum class PixelFormat { Rgba, I420, Gray }

data class MediaInfo(val durationUs: Long, val video: List<VideoStream>, val audio: List<AudioStream>, val tags: Map<String, String>)
data class VideoStream(val codec: String, val profile: String, val width: Int, val height: Int, val rotation: Int, val fps: Double,
    val frames: Int, val bitrate: Int, val transfer: String, val range: String, val primaries: String, val hardware: Boolean)
data class AudioStream(val codec: String, val sampleRate: Int, val channels: Int, val bitrate: Int, val language: String)

data class ExportTarget(val path: String, val container: String, val videoCodec: String, val width: Int, val height: Int, val fps: Double,
    val videoBitrate: Int, val audioCodec: String?, val audioBitrate: Int, val sampleRate: Int, val channels: Int, val encoder: String?)
data class CopyJob(val input: String, val output: String, val startUs: Long, val endUs: Long, val video: Boolean, val audio: Boolean)
data class CompressJob(val input: String, val target: ExportTarget, val startUs: Long, val endUs: Long, val copyAudio: Boolean)
data class EncoderInfo(val name: String, val codec: String, val hardware: Boolean)
data class CaptureDevice(val id: String, val name: String, val audio: Boolean)
data class CaptureMode(val width: Int, val height: Int, val fps: Double)
```

`String?` results are error texts in plain words, `null` for success. Handles are `Long`, zero for failure, as the existing JNI does.

## What each current caller becomes

| Today | Call |
|---|---|
| `ffprobe -show_streams -show_format`, packet lists | `probe` |
| one frame as PNG or raw (`-ss … -frames:v 1`), thumbnails, AI tools reading frames | `openVideo` + `videoFrameAt` / `videoFrame` |
| raw RGBA stream through a pipe (preview fallback, export preview, media viewer) | `openVideo` + `videoFrameAt` |
| audio as raw samples through a pipe (mixer, player, waveforms) | `openAudio` + `readAudio` |
| export: raw frames into ffmpeg's stdin, PCM from a file | `startExport`, `exportVideoFrame` or `exportVideoSurface`, `exportAudio`, `finishExport` |
| audio-only export (WAV, FLAC, M4A, Ogg, MP3) | `startExport` with no video codec |
| Trim, Extract Audio, drop the audio of a download, merge a downloaded video and audio file | `copy` |
| Compress | `compress` |
| Reverse, GIF to video, AI video tools (read frames, write frames back) | `videoFrame` + `startExport` |
| `ffmpeg -encoders` and the two-frame test encode | `encoders` |
| camera through avfoundation, dshow, v4l2 | `cameras`, `openCamera`, `cameraFrame` |

`exportVideoSurface` is the step that makes this better than the pipe: the renderer hands over the GPU surface it just drew
(an IOSurface on macOS, a D3D11 or Vulkan image elsewhere) and the hardware encoder reads it without a read-back.

## Order of the switch

The object is added first with FFmpeg behind it on every platform, so callers move one at a time and nothing changes for users. Then
each platform's back end is replaced when its tests pass. A setting keeps the old path for one release.

## Which decoder a video gets

The system decoder first, FFamp's own or library decoder behind it: VideoToolbox on the Mac, Media Foundation on Windows (H.264, HEVC,
MPEG-2), VA-API on Linux (H.264, HEVC, VP9, AV1), MediaCodec on Android. On Windows and Linux a stream the system decoder refuses, fails
on mid-stream or gives no picture for goes to the software decoder, which keeps it to the end. `VideoDecoder::backend()` names the
decoder in use (`mediafoundation-gpu`, `mediafoundation`, `vaapi`, `h264`, `libde265`, ...).

On Android the app asks MediaCodec first and hands FFamp only what MediaCodec has no decoder for. `AudioDecoder` then opens FFamp's
own FLAC, ALAC, AC-3/E-AC-3 and AAC decoders (besides the WMA, DTS, TrueHD, WavPack, TTA, ADPCM and PCM ones every system has), and
`android::SoftVideo` gives I420 pictures in presentation order from FFamp's own H.264 (every profile: 10-bit, 4:2:2, 4:4:4), ProRes,
DNxHD/DNxHR, DV, VC-1, WMV 7/8, MS-MPEG4, H.263, Cinepak, QuickTime Animation, PNG, HAP, Ut Video, FFV1 and uncompressed decoders
(MPEG-4 part 2 when xvidcore is built). It reads the track's first packets through the `Reader` it is opened with, so H.264 without
reorder information in the stream keeps its first pictures. A picture with `SoftPicture::rgb` set was made from RGB and holds
limited-range BT.709 I420; every other picture keeps the stream's own matrix and range. The Android build has no decoder for MP1/MP2 or AMR sound (mpg123 and opencore-amr are not
built for it), Opus, Vorbis, MPEG-1/2, HEVC, VP8, VP9, AV1, APV or Motion JPEG; MediaCodec has to take those. `ffamp softvideo <file>`
runs SoftVideo on the desktop and writes its pictures to standard output.

Environment variables for tests:

| Variable | Effect |
|---|---|
| `FFAMP_OWN_H264` | FFamp's own H.264 decoder first, the system decoder behind it (Windows and Linux) |
| `FFAMP_NO_SYSTEM_AUDIO` | no system sound decoder: FFamp's own decoders, the route Android takes (every desktop system) |
| `FFAMP_FAIL_SYSTEM_VIDEO=N` | the system video decoder fails from its Nth sample, so the hand-over to the software decoder can be watched |
| `FFAMP_NO_MEDIA_FOUNDATION`, `FFAMP_NO_GPU_DECODE`, `FFAMP_NO_VAAPI` | no Media Foundation (as on Windows N), Media Foundation without the GPU, no VA-API |

## Probe (stands in for ffprobe)

`absent::probeJson(path, packets, &error)` (`include/absent/probe.h`) and `ffamp probe --json [--packets] <file>` give the JSON the app
parses today from `ffprobe -show_streams -show_format [-show_packets] -of json`: the same key names, the same value formats (strings for
times and rates, numbers for sizes and indexes), the same spellings of codec, profile, pixel format and colour names. It uses only the
`Reader` / `Track` interface, so every container a reader exists for is covered.

Deliberate differences from ffprobe (all checked in `tests/suites/40_probe.sh`):

- Packets are listed track by track (ffprobe interleaves them); filter by `stream_index` as the app already does.
- Transport stream audio: `duration` counts every frame (ffprobe stops at the last PES header, about 0.15 s short), `bit_rate` leaves
  out the ADTS headers (ffprobe counts them), `pos` is given for every packet, and a mid-PES `pts` at 44.1 kHz can differ by one 90 kHz
  tick. Transport stream video packet sizes are those of the length-prefixed samples without delimiters.
- Transport streams get `bit_rate` and `nb_frames` per stream (ffprobe has none).
- H.264 in a transport stream: only IDR pictures carry the `K` flag (ffprobe also flags recovery points); the decoder cannot start on
  the others.
- MP4 key flags come from the sync sample table only (ffprobe's parser adds a few more), and `dts` is not shifted down when a file
  has negative composition offsets (`pts` is the same).
- `r_frame_rate` of a file with uneven frame times is the lowest common rate that fits every time stamp; ffprobe's guess can differ
  (2 of 26 real files tried). `avg_frame_rate` matched on all of them.
- MP4 with several separate edits: the segments are joined with each one starting where the one before ends; FFmpeg's joined times
  can leave out one frame step at a join. A single edit at media time 0 on a B-frame track: the video starts at 0 as in ffprobe, and
  its `duration` is the span of the shown frames (ffprobe gives the edit's length).
- Not written: `codec_long_name`, `codec_tag`, `level`, `has_b_frames`, aspect ratios, `disposition`, `handler_name`, per-stream
  `creation_time`, the `displaymatrix` text, `probe_score`, programs and chapters. The app reads none of them.
- AV1: colour names come from the container only (the sequence header is not parsed). VP9 without a first key frame falls back to
  `vpcC`.

## Frame pipes, speech and camera (stand in for the remaining ffmpeg calls)

`include/absent/capture.h` holds `FrameReader`, `FrameWriter`, `convertSpeech` / `shapeSpeech`, `captureDevices`, `captureAccess` and
`CaptureSession`. The commands print or read the same raw bytes the app pipes today, so each call site changes its command line only.

| Today in the app | With ffamp |
|---|---|
| `-ss S -noautorotate -i f -t L -vf fps=R[,scale=W:H] -pix_fmt rgb24 -f rawvideo -` (AI video, matte) | `frames-out f --no-rotate --start S --length L --fps R [--size WxH] --format rgb24` |
| `-ss S -noautorotate -i f -frames:v N -fps_mode passthrough -pix_fmt yuv420p -f rawvideo -` (frame reader) | `frames-out f --no-rotate --start S --count N --format i420` |
| `-ss S -noautorotate -i f [-frames:v 1] -vf scale=W:H -pix_fmt rgba -f rawvideo -` (mosh, interpolation) | `frames-out f --no-rotate --start S [--count 1] --size WxH --format rgba`, or one `FrameReader` kept open (`indexAt`, `read`) |
| `-re [-stream_loop -1] [-ss S] -i f -vf scale=W:H -pix_fmt rgba -f rawvideo -` (export preview, viewer) | `frames-out f [--start S] --size WxH --format rgba --realtime [--loop]` (rotation applied, as FFmpeg does without `-noautorotate`) |
| `-f rawvideo -pix_fmt rgb24/rgba/gray -s WxH -r R -i - [-i src -map 1:a:0?] [setpts ...] -c:v mpeg4 -q:v 2 -c:a aac -t END out.mp4` | `frames-in out.mp4 --size WxH --fps R --format rgb24/rgba/gray [--start S] [--audio-from src] [--audio-start A --audio-length L] [--limit END]` |
| `-af aresample,asetrate,aresample,atempo... -ar 44100 -ac 1 -c:a pcm_s16le out.wav` (speech) | `speech in.aiff out.wav --rate 44100 --speed TEMPO --pitch PITCH` |
| `-f avfoundation -list_devices true -i ""` | `devices` (tab-separated `video|audio`, id, name; two `access` lines first) |
| avfoundation input to an MJPEG pipe plus an H.264/AAC file | `capture [out.mp4] --video ID [--audio ID] [--preview WxH --preview-fps 15] [--seconds N]`, BGRA preview frames on stdout, `q` on stdin stops |

Things a caller has to know:

- `--start S` on `frames-in` keeps the timing of the app's `setpts` line: the first frame written sits at 0 and frame N (N >= 1) at
  `S + (N - 1) / R`. The callers keep writing their extra first frame (the copy in AI video, the black one in the matte) as today.
- `--fps` on `frames-out` follows FFmpeg's `fps` filter and its start rule: each output slot shows the last picture whose time rounds to
  that slot or earlier, a file with more than one track starts at slot 0 (the first picture repeated until its own slot), a file with
  one track starts at the first picture's slot. `--length` keeps `round(L * R)` slots, as `-t` does.
- A start time picks the first picture at or after it (the app passes `pts - 1 us`); times count from the file's own start.
- Scaling is bicubic with chroma centred between luma columns, which is what FFmpeg's scaler does. RGB comes from our own conversion
  (bilinear chroma, matrix from the stream or by height when untagged), so RGB differs from FFmpeg's default fast conversion and is
  close to its `accurate_rnd+full_chroma_int` one. Untagged 720p and larger is read as BT.709 here and as BT.601 by FFmpeg.
- `frames-in` writes H.264 (or HEVC with `--codec hevc`) at 0.8 bits per pixel per frame unless `--bitrate` is given: 22 Mb/s for 720p30,
  50 Mb/s for 1080p30. That is the rate at which it measured at least as good as `mpeg4 -q:v 2` on the same planes. Colours are
  BT.709 limited range and tagged so. An odd width or height is padded by one repeated column or row, because 4:2:0 H.264 and HEVC
  cannot store odd sizes; the stored size is printed.
- Sound from `--audio-from` is copied when it is AAC and starts at 0, otherwise decoded and encoded to AAC (mono stays mono, anything
  else becomes stereo). It ends with the picture or at `--limit`.
- `speech` output has exactly `round(input_samples / speed)` samples, for any speed and pitch from 0.25 to 4 (a speed / pitch ratio
  outside that range is stretched in two steps); a stereo or surround voice file is mixed to mono at 1 / `mixPeak` so it does not
  clip. The app keeps starting the system voice itself.
- `capture` never asks for access unless `--ask` is given: without it a camera or microphone that is not already authorised is an
  error. The ids are AVFoundation unique ids (or `default`); `test` is a built-in test pattern with a tone, the stand-in for the app's
  fake camera. The preview is BGRA at the asked size; without a size it is the camera size, at most 1920 wide.
