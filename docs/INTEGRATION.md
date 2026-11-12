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
