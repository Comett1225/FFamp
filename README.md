# FFamp

Fast Forward Absent Multimedia Project: a small media layer for AbsentMotion's desktop apps that does not use FFmpeg: its own container code, written from the public
specifications, on top of the codecs the operating system already has.

This is a prototype. It proves the riskiest part on macOS; nothing here is used by the app yet.

## What works today (macOS)

- MP4 and MOV reader: plain and fragmented files, H.264 and HEVC video, AAC audio, edit lists, rotation, colour tags.
- MP4 writer: movie header in front (plays while downloading), B-frames, edit lists, gapless audio signalling.
- Matroska and WebM writer with a seek index in front, codec delay and end trimming for audio.
- H.264 and HEVC decoding through VideoToolbox, frames stay on the GPU (IOSurface, Metal-ready).
- Frame-exact seeking to any frame.
- H.264 and HEVC encoding through VideoToolbox, AAC through AudioToolbox, Opus through libopus when it is installed.
- `ffamp`, a command-line tool: `probe`, `decode`, `frames`, `bench`, `encode`, `transcode`, `remux`.
- File tools (`docs/TOOLS.md`): `compress`, `trim`, `extract-audio`, `reverse`, `gif-video`, `merge`, `strip-audio`, `rewrap`, with progress
  lines and a clean stop; `docs/YTDLP.md` describes the downloader without FFmpeg.

## Build and test

    cmake -S . -B build -G Ninja && cmake --build build
    tests/run.sh

The test script needs `ffmpeg`, `ffprobe`, Python 3 with NumPy and the Xcode command line tools. FFmpeg is only the judge: it makes the
test clips and is the reference every frame is compared against. Set `FFMPEG` and `FFPROBE` to use other binaries. Nothing from FFmpeg
is linked or copied.

## Layout

    include/absent/media.h   the whole public interface
    src/mp4_read.cpp         ISO base media file reader (ISO/IEC 14496-12, 14496-14, 14496-15)
    src/mp4_write.cpp        ISO base media file writer
    src/mkv_write.cpp        Matroska and WebM writer (RFC 8794, RFC 9559)
    src/opus_encode.cpp      Opus through libopus (BSD), built only when the library is found
    src/mac/                 VideoToolbox and AudioToolbox back end
    tools/ffamp.cpp         command-line tool
    tests/                   comparison against FFmpeg and against AVFoundation
    docs/                    results and the proposal for how AbsentMotion would call this

## Rules

- Written from the specifications and the operating-system documentation. No code is copied or translated from FFmpeg, GStreamer or any
  other GPL or LGPL project.
- No comments in source files; explanations go in `docs/`.
- Licence: not chosen yet.
