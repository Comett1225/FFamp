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
| Preview decoder (`video.cpp`, D3D11VA or VA-API, copied to memory) | libav linked | `VideoDecoder`: Media Foundation with DXVA where the GPU decodes, else software | `VideoDecoder`: software libraries; VA-API later (see decisions) |
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
| H.264 | VideoToolbox | Media Foundation (DXVA when the GPU has it, software MFT otherwise); OpenH264 when Media Foundation is missing or refuses | OpenH264 (software) | OpenH264: BSD-2-Clause |
| HEVC | VideoToolbox | Media Foundation when the HEVC Video Extensions are installed (DXVA); libde265 otherwise | libde265 | LGPL-3.0-or-later |
| VP8, VP9 | libvpx / system | libvpx | libvpx | BSD-3-Clause (already in FFamp) |
| AV1 | dav1d / system | dav1d | dav1d | BSD-2-Clause (already in FFamp) |
| APV | liboapv | liboapv | liboapv | BSD-3-Clause (already in FFamp) |
| VC-1, WMV7/8, MS-MPEG4 | own | own | own | own |
| MPEG-4 part 2 (DivX, Xvid) | xvidcore | xvidcore | xvidcore | GPL-2.0-or-later (already in FFamp) |
| ProRes | VideoToolbox | own decoder based on FFmpeg's (round 2) | same | own, LGPL-derived (NOTICE) |
| MPEG-1, MPEG-2 | VideoToolbox | Media Foundation MPEG-2 decoder (libmpeg2 for Windows without Media Foundation not built yet) | libmpeg2 0.5.1 (round 2) | GPL-2.0-or-later |
| Motion JPEG, JPEG stills | VideoToolbox / ImageIO | WIC | libjpeg-turbo 3.2.1, the libjpeg API (round 2) | IJG (its SIMD code: zlib) |
| DV | VideoToolbox | own decoder based on FFmpeg's (round 2) | same | own, LGPL-derived |
| H.263 | VideoToolbox | own (the H.263 core FFamp has for WMV and MS-MPEG4, planned) | same | own |
| Lossless video and the rest of the gaps rounds | own | own | own | own |
| AAC (LC, HE, HEv2) | AudioToolbox | Media Foundation AAC decoder; FAAD2 when missing | FAAD2 | GPL-2.0-or-later |
| MP3, MP2, MP1 | AudioToolbox | mpg123 (Media Foundation's MP3 decoder was tried: it places the sound 624 frames later than FFmpeg's) | mpg123 | LGPL-2.1-or-later (already in the Mac dependency list) |
| Opus, Vorbis | libopus, libvorbis | same | same | BSD-3-Clause (already in FFamp) |
| FLAC | AudioToolbox | own decoder written from RFC 9639 (round 1) | same | own |
| ALAC | AudioToolbox | own decoder based on FFmpeg's (round 1) | same | own, LGPL-derived (NOTICE) |
| AC-3, E-AC-3 | AudioToolbox | own decoder based on FFmpeg's (planned; Media Foundation's Dolby decoder is not on every edition) | same | own, LGPL-derived |
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

| Library | Linux `.a` (gcc, with assembly) | Windows `.lib` (MSVC, no assembly yet) | Needed by |
|---|---|---|---|
| OpenH264 decoder | 1.8 MB | 1.1 MB | Linux H.264, Windows fallback |
| libde265 | 1.3 MB | 3.2 MB | Linux HEVC, Windows fallback |
| FAAD2 | 0.6 MB | 0.6 MB | Linux AAC, Windows fallback |
| vo-aacenc | 0.2 MB | 0.3 MB | AAC encode where Media Foundation cannot |
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
| Windows N edition without the Media Feature Pack (no Media Foundation) | everything takes the software path: OpenH264, libde265, FAAD2, mpg123, x264, x265, vo-aacenc. WIC is part of every edition. MPEG-1/2 does not decode there until libmpeg2 is built for Windows. |
| HEVC Video Extensions not installed | HEVC decodes with libde265; HEVC export uses the GPU driver's encoder MFT when there is one, else x265 |
| No GPU video decoder (old GPU, remote desktop, virtual machine) | Media Foundation's software H.264 decoder; HEVC through libde265 |
| GPU encoder MFT refuses the size or is busy | Microsoft's software H.264 MFT, then x264 (the encoder trial picks it as the app does today) |
| Linux without VA-API (the common case, and the default) | software libraries; nothing is asked from the system but libc, libm and libpthread |
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

1. **Patent-pooled codecs built from source** (H.264 through OpenH264 and x264, HEVC through libde265 and x265, AAC through FAAD2
   and vo-aacenc). Built into FFamp they carry the same patent exposure the app already has by bundling FFmpeg, which contains all of
   them. Alternative for H.264 decode only: download Cisco's OpenH264 binary on first use (Cisco pays the royalties), at the cost of a
   download and of the "nothing extra" goal. Default in this plan: build from source, as FFmpeg was.
2. **AAC encoder.** vo-aacenc (LC only) is the software AAC encoder here; FFmpeg's own AAC encoder (what the Linux app uses today) can
   be ported instead if the owner prefers parity over size. Round 1 measures vo-aacenc against FFmpeg.
3. **H.264 on Linux.** OpenH264 cannot decode interlaced (AVCHD 1080i), High 10 or 4:2:2 H.264, which FFmpeg decodes, and round 1
   found that it is not bit-exact on some B-frames of x264 files: a few samples per frame are off by 1 or 2 next to 8x8 edges (its
   B-slice deblocking), 59 to 100 dB against FFmpeg, never a wrong picture. Options: (a) accept OpenH264 as it is, (b) VA-API on GPUs
   that have it (bit-exact, still needs a software path), (c) port FFmpeg's H.264 decoder into FFamp (bit-exact everywhere, several
   rounds, like the Windows Media work). Default for now: (a), with (b) next. On Windows Media Foundation decodes all of it
   bit-exact, interlaced included; High 10 and 4:2:2 fail there too.
4. **HEVC software export** costs x265 (4 to 8 MB). Alternative: offer HEVC export only where a hardware encoder exists. Default:
   ship x265, as the BtbN FFmpeg build does.
5. **HE-AAC v2 (parametric stereo) on Linux.** FAAD2's parametric stereo differs from FFmpeg's (17.6 dB left, 9.5 dB right on the
   suite's file; Apple's decoder matches FFmpeg to 100 dB). Plain AAC and HE-AAC v1 match FFmpeg at 130 dB or better. Options: accept,
   or port FFmpeg's AAC decoder later. Windows uses Media Foundation's decoder.
6. **NASM on Windows.** The Windows libraries are built without assembly because the PC has no NASM; dav1d, libvpx, x264, x265 and
   SVT-AV1 are several times slower without it. Media Foundation covers H.264 and HEVC, so this matters for VP9, AV1 and the software
   fallbacks. Fetching NASM (one program, from its own site) would fix it; not done without the owner's go.

## Round 1 (this branch)

1. This plan.
2. Linux build: the desktop layer, every portable reader, decoder, writer and tool, the CLI, a static `libffamp.a`; suites in WSL.
3. Windows build with clang-cl, same suites.
4. Windows backends: Media Foundation decode (H.264, HEVC, AAC, MP3), encode (H.264, HEVC, AAC), WIC stills, each with its software
   path, each checked against ffmpeg.
5. Linux backends: H.264, HEVC, AAC decode and encode, VP9, AV1, checked against ffmpeg.
6. Speed against ffmpeg on both systems (`docs/RESULTS.md`).

Later rounds: ProRes, DV, H.263, MPEG-2, AC-3, AMR, Motion JPEG on Linux, capture on both systems, VA-API, NASM builds on Windows,
DXVA zero-copy into the app's D3D11 path, then the app wiring (round 2 of the app work).
