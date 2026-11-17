# FFamp on Windows and Linux

Goal: AbsentMotion on Windows and Linux installs as smoothly with FFamp as with FFmpeg: one download, nothing extra to install, every
job FFmpeg does for the desktop app done by FFamp. FFamp stays an opt-in media engine in the app until about 5.5; this plan is about
FFamp itself. Scope: only what FFmpeg does for the app (the file types `Desk.kt` accepts: mp4, mov, m4v, webm, mkv, avi, wmv, asf, mp3,
wav, m4a, aac, ogg, opus, flac, wma, plus GIF to video).

## Shape of the port

- **One desktop layer for Windows and Linux** (`src/desktop/`): `VideoDecoder`, `VideoEncoder`, `AacEncoder`, the system audio
  decoder hook, `Scaler`, still pictures and the frame copies, written once over FFamp's existing seams (`SoftDecoder`, `SoftEncoder`,
  `PacketDecoder`). Frames are CPU pictures (`Frame.native` holds planar 8 or 16-bit YUV with the decoder's own layout), which is what
  the app's Windows and Linux preview uses today: its libav path copies D3D11VA and VA-API frames back to memory before Skia
  (`VideoHwImage` returns nothing there).
- **Platform backends** behind those seams: `src/windows/` (Media Foundation, WIC, Media Foundation capture), `src/linux/` (VA-API
  and V4L2 when they come). Each backend is picked at run time and falls back to software when the system part is missing.
- **Software libraries** are built static from pinned sources by `third_party/build_linux.sh` and `third_party/build_windows.sh` (the
  same scheme as `build_mac.sh`), so the app ships one file with no outside dependency.
- Media Foundation is loaded at run time (`LoadLibrary` of `mfplat.dll`, `mf.dll`), never linked: on Windows N editions without the
  Media Feature Pack the library still loads and every job takes the software path.
- The suites run on all three systems; FFmpeg stays only as the judge in tests.

## Every job FFmpeg does for the desktop app, and the FFamp path

Read from `FfmpegEngine.kt` (command lines), `pc/native/video.cpp` (libav preview decoder) and `video_copy.cpp` (hardware device).
"Own" = FFamp's own code, the same on every system. The containers (MP4/MOV, Matroska/WebM, AVI, ASF, Ogg, WAV/AIFF/CAF, MP3, ADTS,
FLAC, IVF, MPEG-TS) and the writers (MP4/MOV/M4A, Matroska/WebM, Ogg, WAV, FLAC, MP3) are own code everywhere.

| Job (app call) | FFmpeg today | Windows | Linux |
|---|---|---|---|
| Probe (`probe`, `DeskProbe`, `DeskMedia.probe`) | ffprobe JSON | own (`probeJson`) | own |
| Packet list (`packets`, `MediaExtractor.index`) | ffprobe packets | own readers | own readers |
| Thumbnail (`still`) | ffmpeg decode, scale, PNG | `FrameReader` over the decoders below | same |
| Sound for the mix and viewer (`pcm`, `playback`) | ffmpeg decode + aresample | `PcmReader` (own resampler) over the audio decoders below | same |
| I420 frames (`yuv`), RGBA frames (`play`, `videoSource`, `steadyFrames`) | ffmpeg rawvideo pipes | `FrameReader` (own scaler) | same |
| Preview decoder (`video.cpp`, D3D11VA or VA-API, copied to memory) | libav linked | `VideoDecoder`: Media Foundation with DXVA for H.264, HEVC and MPEG-2 first; FFamp's own H.264 decoder and libde265 for what it refuses or fails on; else software | `VideoDecoder`: VA-API for H.264, HEVC, VP9 and AV1 where a driver loads (round 2), else software libraries |
| Encoder list and trial (`encoders`, `encoderWorks`) | `ffmpeg -encoders`, 2-frame trial | `encoders()`, `encoderWorks()` | same |
| Video export (H.264, HEVC, AV1, VP9, APV; AAC, Opus; MP4, MOV, WebM, MKV) | `h264_nvenc/qsv/amf/mf`, `libx264`, `libx265`, `libsvtav1`, `libvpx-vp9`, `liboapv`, `aac_mf`, `aac`, `libopus` | Media Foundation H.264/HEVC (hardware encoder MFT first, then Microsoft's software H.264 MFT), x264 / x265 when Media Foundation cannot; SVT-AV1, libvpx, liboapv, libopus; AAC: Media Foundation (96 to 192 kbit/s), else vo-aacenc | x264, x265, SVT-AV1, libvpx, liboapv, libopus, vo-aacenc (VA-API encode later) |
| Audio export (WAV 16/24, FLAC, M4A, Ogg Opus, MP3) | pcm, flac, aac, libopus, libmp3lame | own WAV and FLAC, AAC as above, libopus, LAME | own, vo-aacenc, libopus, LAME |
| Export check (`frameTimes`) | ffprobe frames/packets | own (`times`) | own |
| Compress, extract audio, trim, strip audio, merge, rewrap | ffmpeg tools | own tools (`include/absent/tools.h`) over the decoders and encoders above | same |
| Reverse, AI video, matte, clip smooth (`reverse`, `frameSink`) | MPEG-4 part 2 + AAC | own tools, H.264 through `FrameWriter` | same |
| GIF to video (`gifVideo`) | ffmpeg GIF demuxer + x264 | own GIF reader + H.264 encoder | own GIF reader + x264 |
| Speech post-processing (`speech`) | aresample, asetrate, atempo | own (`convertSpeech`) | own |
| Camera list, preview, recording (`cameraDevices`, `camera`) | dshow input, MJPEG pipe, `h264_mf`/`libx264` | Media Foundation capture source reader + the H.264/AAC encoders | V4L2 + PipeWire/PulseAudio (dlopen) + x264 |
| Downloader merge (`downloadOptions`, `withoutAudio`) | `--ffmpeg-location` | own `merge`, `extractAudio`, `rewrap`, `stripAudio` (docs/YTDLP.md) | same |

### Decoders behind those jobs

| Codec | Mac today | Windows | Linux | Licence of the added part |
|---|---|---|---|---|
| H.264 | VideoToolbox | Media Foundation (DXVA when the GPU has it) first (owner decision 2026-11-17: system decoders first); FFamp's own decoder (FFmpeg's, ported) for streams Media Foundation refuses or fails on, and without Media Foundation | VA-API for 8-bit 4:2:0 (round 2); FFamp's own decoder based on FFmpeg's, with its assembly and frame threads, otherwise | own, LGPL-derived (NOTICE) |
| HEVC | VideoToolbox | Media Foundation when the HEVC Video Extensions are installed (DXVA); libde265 otherwise | VA-API (round 2); libde265 otherwise | LGPL-3.0-or-later |
| VP8, VP9 | libvpx / system | libvpx | VA-API for VP9 (round 2); libvpx | BSD-3-Clause (already in FFamp) |
| AV1 | dav1d / system | dav1d | VA-API (round 2); dav1d | BSD-2-Clause (already in FFamp) |
| APV | liboapv | liboapv | liboapv | BSD-3-Clause (already in FFamp) |
| VC-1, WMV7/8, MS-MPEG4 | own | own | own | own |
| MPEG-4 part 2 (DivX, Xvid) | xvidcore | xvidcore | xvidcore | GPL-2.0-or-later (already in FFamp) |
| ProRes | VideoToolbox | own decoder based on FFmpeg's (round 2) | same | own, LGPL-derived (NOTICE) |
| MPEG-1, MPEG-2 | VideoToolbox | Media Foundation MPEG-2 decoder (libmpeg2 for Windows without Media Foundation not built yet) | libmpeg2 0.5.1 (round 2) | GPL-2.0-or-later |
| Motion JPEG, JPEG stills | VideoToolbox / ImageIO | WIC | libjpeg-turbo 3.2.1, the libjpeg API (round 2) | IJG (its SIMD code: zlib) |
| DV | VideoToolbox | own decoder based on FFmpeg's (round 2) | same | own, LGPL-derived |
| H.263 | VideoToolbox | own decoder on the H.263 core FFamp has for WMV and MS-MPEG4, based on FFmpeg's (round 2) | same | own, LGPL-derived (NOTICE) |
| Lossless video and the rest of the gaps rounds | own | own | own | own |
| Raw video in AVI (RGB, palettes, YUV layouts), Cinepak | own | own | own | own, LGPL-derived (NOTICE) |
| AAC (LC, HE, HEv2) | AudioToolbox | Media Foundation AAC decoder; FFamp's own decoder based on FFmpeg's when missing | own decoder based on FFmpeg's | own, LGPL-derived (NOTICE) |
| MP3, MP2, MP1 | AudioToolbox | mpg123 (Media Foundation's MP3 decoder was tried: it places the sound 624 frames later than FFmpeg's) | mpg123 | LGPL-2.1-or-later (already in the Mac dependency list) |
| Opus, Vorbis | libopus, libvorbis | same | same | BSD-3-Clause (already in FFamp) |
| FLAC | AudioToolbox | own decoder written from RFC 9639 (round 1) | same | own |
| ALAC | AudioToolbox | own decoder based on FFmpeg's (round 1) | same | own, LGPL-derived (NOTICE) |
| AC-3, E-AC-3 | AudioToolbox | own decoder based on FFmpeg's (Media Foundation's Dolby decoder is not on every edition) | same | own, LGPL-derived |
| AMR-NB, AMR-WB | AudioToolbox | opencore-amr | opencore-amr | Apache-2.0 |
| WMA family, ADPCM, G.711, TTA, DTS, TrueHD, WavPack | own | own | own | own |

### Encoders

| Codec | Mac today | Windows | Linux | Licence |
|---|---|---|---|---|
| H.264 | VideoToolbox | Media Foundation (hardware MFT of AMD, Intel or NVIDIA first, then Microsoft's software MFT); x264 when Media Foundation is missing | x264 | GPL-2.0-or-later |
| HEVC | VideoToolbox | Media Foundation hardware MFT (registered by the GPU driver) or the HEVC Video Extensions; x265 otherwise | x265 | GPL-2.0-or-later |
| AV1, VP9, APV | SVT-AV1, libvpx, liboapv | same | same | BSD (already in FFamp) |
| AAC | AudioToolbox | Media Foundation AAC encoder at 96, 128, 160 and 192 kbit/s (its only rates); vo-aacenc for other rates and when missing | vo-aacenc | Apache-2.0 |
| Opus, MP3, FLAC, WAV | libopus, LAME, own | same | same | already in FFamp |

### Added binary size (static, x86-64, measured in round 1)

| Library | Linux `.a` (gcc, with assembly) | Windows `.lib` (MSVC, measured before NASM; with assembly since round 2) | Needed by |
|---|---|---|---|
| FFamp's H.264 decoder (`src/h264`, part of FFamp) | 1.4 MB of code and tables, with the assembly | about the same (clang-cl, with the assembly) | Linux H.264, Windows fallback |
| libde265 | 1.3 MB | 3.2 MB | Linux HEVC, Windows fallback |
| vo-aacenc | 0.2 MB | 0.3 MB | AAC encode where Media Foundation cannot |
| opencore-amr (decoders) | 0.4 MB | 0.4 MB | AMR-NB and AMR-WB decode on both systems |
| mpg123 | 0.5 MB | 0.7 MB | MP1/MP2/MP3 decode on both systems |
| x264 (8-bit) | 1.3 MB | 2.0 MB | Linux H.264 encode, Windows fallback |
| x265 (8-bit) | 7.9 MB | 4.2 MB | Linux HEVC encode, Windows fallback |
| libmpeg2 | 0.2 MB | not built (Media Foundation) | Linux MPEG-1/2 decode |
| libjpeg-turbo | 1.2 MB | not built (WIC) | Linux Motion JPEG and JPEG decode |
| libvpx, dav1d, SVT-AV1, libopus, libvorbis, libogg, LAME, liboapv, xvidcore | 22 MB together | 14 MB together | all systems |

Whole programs: the FFamp command-line tool with every library linked in is 11.6 MB on Windows and 25.5 MB on Linux (stripped).
The FFmpeg the app bundles today is about 190 MB on Windows (the seven DLLs, ffmpeg.exe and ffprobe.exe) and about 200 MB of shared
libraries on Linux. Archive sizes overstate what a link keeps; the app's own library will be smaller than the sum.

## What happens on a PC without the optional system part

| Missing part | Effect |
|---|---|
| Windows N edition without the Media Feature Pack (no Media Foundation) | everything takes the software path: FFamp's H.264 decoder, libde265, FFamp's own AAC decoder, mpg123, x264, x265, vo-aacenc. WIC is part of every edition. MPEG-1/2 does not decode there until libmpeg2 is built for Windows. |
| HEVC Video Extensions not installed | HEVC decodes with libde265; HEVC export uses the GPU driver's encoder MFT when there is one, else x265 |
| No GPU video decoder (old GPU, remote desktop, virtual machine) | H.264 through Microsoft's decoder on the processor (backend `mediafoundation`); HEVC through libde265 |
| GPU encoder MFT refuses the size or is busy | Microsoft's software H.264 MFT, then x264 (the encoder trial picks it as the app does today) |
| Linux without VA-API (no libva, no driver, NVIDIA's driver) | software libraries; nothing is asked from the system but libc, libm and libpthread (libva is only tried with `dlopen`) |
| Linux without PipeWire or PulseAudio | camera recording without sound |

## Licences

- FFamp is GPL-3.0-or-later. Every added library is GPL-3.0 compatible: BSD-2/3, Apache-2.0, LGPL-2.1/3-or-later, GPL-2.0-or-later.
- No non-free code: no fdk-aac, no Cisco binary blob, no proprietary codec SDK. Media Foundation and WIC are system libraries
  (the GPL's system library exception).
- Decoders written from FFmpeg's code (ProRes, DV, FLAC, ALAC, AC-3, H.263) get one line each in `docs/NOTICE.md`, as before.
- libjpeg-turbo's IJG licence asks that the documentation of a program shipped without its source say "this software is based in
  part on the work of the Independent JPEG Group"; that line goes into the app's notices with the licence texts.
- Each bundled library's licence text goes next to the app's notices (round 3 of the app work, as on Mac).

## Decisions for the owner

1. **Patent-pooled codecs built from source** (H.264 through FFamp's own decoder and x264, HEVC through libde265 and x265, AAC through
   FFamp's own decoder and vo-aacenc, AMR through opencore-amr). Built into FFamp they carry the same patent exposure the app already has
   by bundling FFmpeg, which contains all of them. (An alternative once considered for H.264 decode only: download Cisco's OpenH264
   binary on first use, at the cost of a download and of the "nothing extra" goal.) Default in this plan: build from source, as FFmpeg was.
2. **AAC encoder.** vo-aacenc (LC only) is the software AAC encoder here; FFmpeg's own AAC encoder (what the Linux app uses today) can
   be ported instead if the owner prefers parity over size. Round 1 measures vo-aacenc against FFmpeg.
3. **H.264 on Linux.** Decided 2026-11-16: FFamp's own decoder, FFmpeg's H.264 decoder ported to `src/h264` with its x86-64 and
   AArch64 assembly and its frame and slice threads. It replaced OpenH264 (single-threaded, a few values off on some B-frames, no
   interlaced, 8-bit 4:2:0 only); bit-exact with FFmpeg, faster than FFmpeg's own decode with threads (`docs/RESULTS.md`). VA-API (round 2)
   decodes 8-bit 4:2:0 H.264 on the GPU where a driver loads; every refusal there goes to FFamp's own decoder. On Windows Media
   Foundation comes first (owner decision 2026-11-17, system decoders first on every system; from 2026-11-16 to then FFamp's own decoder
   came first) and FFamp's own decoder takes the streams it refuses or fails on, also mid-stream.
4. **HEVC software export** costs x265 (4 to 8 MB). Alternative: offer HEVC export only where a hardware encoder exists. Default:
   ship x265, as the BtbN FFmpeg build does.
5. **HE-AAC v2 (parametric stereo) on Linux.** Decided (2026-11-15): FFmpeg's AAC decoder is ported (LC, HE-AAC, HE-AAC v2), FAAD2
   is gone. It is as close to FFmpeg's output as FFmpeg's own C code is to its SIMD code (RESULTS, round 2).
6. **NASM on Windows.** Done in round 2 with the owner's go: the portable zip from nasm.us, a build tool only.
7. **NVIDIA on Linux (not built).** NVIDIA's driver has no VA-API. NVDEC would take: `libcuda.so` and `libnvcuvid.so` (shipped with
   the driver) loaded with `dlopen`, the MIT-licensed `nv-codec-headers` for the declarations, a CUDA context per process, and per
   codec either NVIDIA's own parser (`cuvidCreateVideoParser` takes the Annex B stream and calls back with ready picture parameters:
   about 400 lines for H.264, HEVC, VP9 and AV1 together) or FFamp's VA-API parsers mapped onto `CUVIDPICPARAMS` (about 300 lines
   per codec, FFamp's own handling of missing references); pictures come back through `cuvidMapVideoFrame` and `cuMemcpy2D` as NV12
   or P016. Testing needs an NVIDIA GPU (none here); under WSL the CUDA libraries come from `/usr/lib/wsl/lib`.

## Round 1 (this branch)

1. This plan.
2. Linux build: the desktop layer, every portable reader, decoder, writer and tool, the CLI, a static `libffamp.a`; suites in WSL.
3. Windows build with clang-cl, same suites.
4. Windows backends: Media Foundation decode (H.264, HEVC, AAC, MP3), encode (H.264, HEVC, AAC), WIC stills, each with its software
   path, each checked against ffmpeg.
5. Linux backends: H.264, HEVC, AAC decode and encode, VP9, AV1, checked against ffmpeg.
6. Speed against ffmpeg on both systems (`docs/RESULTS.md`).

## Round 2, video (branch `wl-video`)

1. NASM builds of the Windows libraries with assembly.
2. Windows decode speed: FFamp's own staging copies from the GPU, pictures split and copied on the pixel crew, no copy for pictures
   that are not kept, one D3D11 device per process, Microsoft's decoders created directly and without their affinity stall.
3. VA-API on Linux for H.264, HEVC, VP9 and AV1, loaded at run time, tested under WSL with Mesa's d3d12 driver.
4. MPEG-1/2 (libmpeg2) and Motion JPEG (libjpeg-turbo) on Linux; ProRes, DV and H.263 as own decoders on both systems.
5. Full suites and speed tables (`docs/RESULTS.md`).

## Round 2, audio (branch `wl-audio`) and FFamp's own H.264 decoder (branch `wl-h264`)

1. FFamp's own AAC decoder based on FFmpeg's (LC, HE-AAC, HE-AAC v2) in place of FAAD2; own AC-3 and E-AC-3; AMR through opencore-amr.
2. FFmpeg's H.264 decoder ported to `src/h264` (assembly, frame and slice threads) in place of OpenH264 on Linux and behind Media
   Foundation on Windows.
3. The three branches merged into `winlinux`, full suites and speed tables on both systems (`docs/RESULTS.md`).

## Follow-up (branch `wl-followup`)

1. Windows: FFamp's own H.264 decoder first, Media Foundation as the fallback (the import patch that keeps Microsoft's decoder away
   from `SetThreadAffinityMask` stays); speed, seeks, thumbnails and real time at 1080p60, 4K30 and 4K60 with processor use.
2. 6.1 sound off the Mac in FFmpeg's order (decoders, mixer, checks); the Mac unchanged.
3. FFmpeg's FATE h264 conformance set against FFamp's own decoder on Windows and Linux, as a suite that runs when the folder is there.
4. FFamp links next to FFmpeg in one library (no `av_`/`ff_` names), and the encoder list names the Windows and Linux encoders.

## Media Foundation first (branch `mf-first`)

1. Windows: Media Foundation first for H.264 again, FFamp's own decoder behind it for streams Media Foundation refuses, fails on
   mid-stream or gives no picture for, and without Media Foundation; `FFAMP_OWN_H264` for the other order. The same hand-over serves
   every system decoder (Media Foundation HEVC to libde265, VA-API to the software decoders).
2. Media Foundation pictures keep the stream's left and top cropping (display aperture).
3. FATE h264 conformance through `VideoDecoder` in both orders.
4. System decoders this Windows has that FFamp does not use yet (RESULTS): VP9 and VP8 (VP9 Video Extensions), AV1 (AV1 Video
   Extension), MPEG-4 part 2 and H.263, MS-MPEG4 v2 and v3, WMV1/2/3 and VC-1, DV; FLAC, ALAC, Opus, AMR-NB and the WMA family for
   sound. Each needs its own input setup and a check against FFmpeg; not started.

Later rounds: capture on both systems, NVDEC, DXVA zero-copy into the app's D3D11 path, libmpeg2 for Windows without Media
Foundation, then the app wiring (round 2 of the app work).
