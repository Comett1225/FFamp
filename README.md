# FFamp

Fast Forward Absent Multimedia Project: AbsentMotion's own media layer, used instead of FFmpeg: its own container code, written from the
public specifications, on top of the codecs the operating system already has, plus its own decoders for formats the system cannot play.

AbsentMotion uses it on phones for videos and sound the phone cannot play (AVI, WMV, AC-3 sound, 10-bit and 4:2:2 H.264, ProRes,
DNxHD), and on computers as the experimental media reader (Settings > Playback > Media reader).

## What works today (macOS)

- MP4 and MOV reader: plain and fragmented files, H.264 and HEVC video, AAC audio, edit lists, rotation, colour tags.
- MP4 writer: movie header in front (plays while downloading), B-frames, edit lists, gapless audio signalling.
- Matroska and WebM writer with a seek index in front, codec delay and end trimming for audio.
- H.264 and HEVC decoding through VideoToolbox, frames stay on the GPU (IOSurface, Metal-ready).
- Frame-exact seeking to any frame.
- H.264 and HEVC encoding through VideoToolbox, AAC through AudioToolbox, Opus through libopus when it is installed.
- VC-1 (Windows Media Video 9: `WMV3` with Complex profile and its X8 intra pictures, `WVC1`, `WMVA`) through FFamp's own software
  decoder, based on FFmpeg's (`docs/NOTICE.md`).
- Microsoft MPEG-4 v1 to v3 (`MPG4`, `MP42`, `MP43`, the DivX 3 AVIs), Windows Media Video 7 and 8 (`WMV1`, `WMV2`, with X8 intra
  pictures) through FFamp's own decoder, based on FFmpeg's.
- Windows Media Audio 1, 2, 9 Professional, 9 Lossless and 9 Voice through FFamp's own decoders, based on FFmpeg's.
- A-law, mu-law, IMA ADPCM (WAV, QuickTime, DK3, DK4), Microsoft and Yamaha ADPCM, G.722, G.726 and TTA through FFamp's own decoders,
  based on FFmpeg's, in WAV, AIFF-C, CAF, AVI, Matroska, QuickTime and ASF; FLAC, AAC, MP3, AC-3 and E-AC-3 in the WAV, AVI and CAF
  layouts FFmpeg writes.
- AVI as FFmpeg reads it (OpenDML over 4 GB, idx1, no or broken index, odd chunks and JUNK, DV type-1 with its sound, avc1 H.264,
  ADTS AAC); raw video (RGB, palettes, YUV layouts) and Cinepak through FFamp's own decoders, based on FFmpeg's.
- `ffamp`, a command-line tool: `probe`, `decode`, `frames`, `bench`, `encode`, `transcode`, `remux`.
- File tools (`docs/TOOLS.md`): `compress`, `trim`, `extract-audio`, `reverse`, `gif-video`, `merge`, `strip-audio`, `rewrap`, with progress
  lines and a clean stop; `docs/YTDLP.md` describes the downloader without FFmpeg.

## Windows and Linux

The same library and tool build on Windows (clang-cl) and Linux (gcc or clang) through one desktop layer (`src/desktop`): Media
Foundation and WIC on Windows, libraries built static from pinned sources everywhere else; what each job uses on each system, and why,
is in `docs/PLAN_WINLINUX.md`, the details in `docs/CONVENTIONS.md`, the measurements in `docs/RESULTS.md`.

    third_party/build_linux.sh deps/linux                        Linux libraries (static, PIC)
    third_party/build_windows.sh deps/windows                    Windows libraries (Git Bash inside a vcvars64 shell)
    cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=deps/linux   (on Windows add -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl)

## Build and test

    cmake -S . -B build -G Ninja && cmake --build build
    tests/run.sh

The test script needs `ffmpeg`, `ffprobe`, Python 3 with NumPy and, on the Mac, the Xcode command line tools (on Windows it runs in Git
Bash; `FFAMP_CMAKE` passes extra CMake arguments). FFmpeg is only the judge: it makes the
test clips and is the reference every frame is compared against. Set `FFMPEG` and `FFPROBE` to use other binaries. Nothing from FFmpeg
is linked; parts based on FFmpeg are listed in `docs/NOTICE.md`.

## Layout

    include/absent/media.h   the whole public interface
    src/mp4_read.cpp         ISO base media file reader (ISO/IEC 14496-12, 14496-14, 14496-15)
    src/mp4_write.cpp        ISO base media file writer
    src/mkv_write.cpp        Matroska and WebM writer (RFC 8794, RFC 9559)
    src/opus_encode.cpp      Opus through libopus (BSD), built only when the library is found
    src/vc1_*.cpp            VC-1 decoder (based on FFmpeg's, see docs/NOTICE.md)
    src/h263.*, msmpeg4*.cpp Microsoft MPEG-4 v1 to v3, WMV7 and WMV8 on the shared H.263-family core (based on FFmpeg's)
    src/intrax8.*            X8 intra pictures of WMV8 and VC-1 Complex profile (based on FFmpeg's)
    src/mac/                 VideoToolbox and AudioToolbox back end
    src/desktop/             Windows and Linux: decoder, encoder, scaler, GIF reader on CPU frames
    src/windows/             Media Foundation and WIC back end
    tools/ffamp.cpp         command-line tool
    tests/                   comparison against FFmpeg and against AVFoundation
    docs/                    results and the proposal for how AbsentMotion would call this

## Rules

- FFamp is inspired by FFmpeg.
- Written from the specifications and the operating-system documentation. Parts based on FFmpeg are listed in `docs/NOTICE.md`.
- No comments in source files; explanations go in `docs/`.
- Licence: GPL-3.0-or-later (`LICENSE`).
