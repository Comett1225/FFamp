# Prototype results

Measured on an Apple-silicon Mac with `tests/run.sh` (31 checks, run twice). The reference is FFmpeg 9.0.2. Times are whole-process
wall time, best of three; where two runs differed, both numbers are given.

## Correctness

| Check | Result |
|---|---|
| Every frame of six clips (H.264 and HEVC, 720p and 1080p, MP4, MOV, fragmented MP4) against FFmpeg's decode | 4,800 of 4,800 frames byte-identical |
| The app's own test clips (720p H.264, 4K HEVC) | 300 of 300 frames byte-identical |
| Seek to 50 random frames per clip, four clips, in random order | 200 of 200 byte-identical |
| Our MP4 files (H.264 + AAC, HEVC + AAC) in `ffprobe -v error` and a full FFmpeg decode | no messages |
| Our MP4 files in AVFoundation (the engine behind QuickTime Player): playable, every frame read, a still at the middle | pass, 300 of 300 frames, audio 480,000 of 480,000 samples |
| Audio after AAC encode, decoded by FFmpeg, against the source | same length to the sample, 49 dB signal-to-noise |
| Our Matroska files (H.264 + Opus, HEVC + AAC) in `ffprobe -v error`, a full FFmpeg decode, and a seek through our index | no messages; the frame after the seek is identical |
| Opus audio in Matroska, decoded by FFmpeg, against the source | same length to the sample, 42 dB signal-to-noise |
| Stream copy (remux) of H.264 and HEVC files | every packet identical |
| Fragmented MP4 turned into a plain MP4 | clean, every frame identical |
| Picture quality of our encode against FFmpeg's VideoToolbox encode, same bitrate | 46.1 dB against 46.7 dB (H.264), 42.6 dB against 43.0 dB (HEVC), files 4 to 5 % smaller |

## Speed: decoding a 1080p, 60 second clip (1,800 frames)

| Path | H.264 | HEVC |
|---|---|---|
| ours, frames stay on the GPU | 1,980 to 1,994 fps | 2,657 to 2,665 fps |
| ours, frame by frame in display order, on the GPU | 829 to 913 fps | 1,649 to 1,664 fps |
| ours, frame by frame, copied to memory | 744 to 841 fps | 1,389 to 1,465 fps |
| ffmpeg CLI, VideoToolbox, frames stay on the GPU | 318 to 346 fps | 481 to 570 fps |
| ffmpeg CLI, software decode, no output | 1,231 to 1,289 fps | 638 to 908 fps |
| ffmpeg CLI, software decode, raw RGBA through a pipe | 476 to 479 fps | 366 to 441 fps |

## Speed: seeking to a random frame (1080p)

| Path | H.264 (key frame every 2 s) | HEVC (key frame every 8 s) |
|---|---|---|
| ours, in process, mean of 50 | 15 ms | 44 to 45 ms |
| ffmpeg CLI started per seek, mean of 20 | 106 to 125 ms | 204 to 210 ms |

## Speed: encoding 300 frames of 1080p

| Path | H.264 | HEVC |
|---|---|---|
| ours: VideoToolbox + AAC + own MP4 writer | 213 fps | 203 fps |
| ffmpeg CLI: the same hardware encoder | 206 fps | 191 fps |

Compressing the 60 second clip to HEVC with the audio copied: 213 fps against 205 fps. The hardware encoder sets the pace on both
sides, so export speed is the same; the gain is in decoding, seeking and in not starting a process.

## What the tests found

- Apple's reader cut 40 ms off the end of the audio until the file carried a pre-roll sample group and a shortened last packet next to
  the edit list. FFmpeg's reader did not show the problem. Both now agree to the sample.
- A decoder seek that restarts at a key frame must throw away the frames before the target as they arrive, or a long group of pictures
  holds hundreds of megabytes for a moment. Peak memory for a seek that decodes 250 frames is now 42 MB.

## Not covered by the prototype

10-bit and HDR, interlaced video, more than two audio channels, AAC decoding, MOV writing, Matroska reading, files with several video
sample descriptions, edit lists with more than one segment, edit lists in fragmented files, negative timestamps when copying into
Matroska, and fuzz testing of the readers. Players other than Apple's engine (VLC, browsers, Windows) have not been tried yet.
