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

## File tools (`docs/TOOLS.md`)

`tests/suites/70_tools.sh`: 87 checks that need no FFmpeg (fixtures in `tests/fixtures/tools/`, 290 KB) and 63 more against the files the
app's FFmpeg command lines produce for the same inputs.

| Check | Result |
|---|---|
| Trim: packets between the key frame at or before the start and the end | all byte-identical (MP4, Matroska, WebM, transport stream in); length exact to the frame, sound exact to the sample |
| Trim against `ffmpeg -ss -t -c copy` | same first key frame; every one of our packets is among FFmpeg's; ours ends at the asked time, FFmpeg's 0.07 s later with the picture 0.083 s behind the sound |
| Extract by copy | every packet identical, also to FFmpeg's copy |
| Extract to AAC 96 kbit/s and MP3 128 kbit/s | length within 0.03 s, bitrate within 20 %, sound agrees with the source and with FFmpeg's file (correlation above 0.97) |
| Compress (cut, 160x90, 15 fps; 60 fps; HEVC; VP9; AV1; 10-bit source; variable frame rate source; 30000/1001) | size, frame count, constant frame times, length, bitrate within 35 % on the 2 to 4 second test clips (within 0.2 % on a 60 second 1080p clip), the right source frame in every slot |
| Compress against the app's FFmpeg command | same frame count and frame times to the millisecond, file size within 30 %, picture quality against the source not below FFmpeg's |
| Reverse | frame N equals source frame (last - N) at 49 dB or better (55 dB for the variable frame rate clip), every frame shown as long as in the source, sound is the source backwards (correlation 1.0000), also through the scratch file |
| Reverse against the app's FFmpeg steps | same frame count, length, frame times; pictures agree at 36 dB or better |
| GIF frames from ImageIO against an independent drawing (five files) | every frame identical to the byte, colour and alpha |
| GIF videos | sizes, frame times and length follow the delays; colour is the premultiplied drawing (53 dB or better), matte is the alpha (57 dB) |
| Merge, strip-audio, transport stream to MP4 | every packet kept, also identical to FFmpeg's merge; gap between picture and sound kept to the millisecond |
| Stop with SIGINT or SIGTERM | exit code 130, no output, `.part` or scratch file |
| Cut-off and scrambled inputs (56 runs in the suite, 3,120 more by hand) | an error or a file, never a crash or a hang |
| Every output | opens in AVFoundation; no ffprobe or FFmpeg decode messages |

Speed on a 60 second 1080p H.264 clip (wall time, one run each, another job was running on the machine):

| Job | Ours | FFmpeg, same job |
|---|---|---|
| Compress to 720p, 30 fps, 4 Mbit/s, AAC | 5.6 to 6.8 s, 52 MB peak | 4.4 s, 180 MB peak |
| Compress to HEVC 1080p 5 Mbit/s, sound copied | 8.7 s, 46 MB | 9.0 s, 204 MB |
| Reverse the whole clip | 13.8 s, 57 MB of process memory plus up to 512 MB of GPU frames | not measured |
| Reverse with a 64 MB frame budget (30 groups through the scratch file) | 35 s | |
| Copy every packet into a new MP4 (62 MB) | 0.21 s | |
| Sound to MP3 | 0.29 s | |

Compress with scaling is slower than FFmpeg because decoding, scaling and encoding run one after the other on one thread here.

# Proof round: probe parity, fuzzing, one real project with no FFmpeg

## Probe against ffprobe

`probeJson` now gives ffprobe's values for Matroska and WebM (1/1000 time base, the length the file states, no `und` language, `eng` when
none is stored, frame rate from the presentation times), Ogg Opus (length counts the pre-skip, as ffprobe does), WAV and ADTS (no start
time), MP3 (1/14112000 time base, start time = encoder delay) and PCM in MOV/AIFF (`nb_frames` counts sample frames). 47 committed
reader, audio and video fixtures are now compared with ffprobe field by field next to the 26 probe fixtures.

Known differences, kept on purpose: ffprobe reads only the first link of a chained Ogg file and puts Ogg tags on the stream; it estimates
the length of ADTS and VBRI files from the bit rate where ours counts the packets; it gives no usable length for IVF; Apple-encoded and
HE-AAC M4A files get a start time and a longer length from ffprobe because it does not apply their edit list the way a player does; 8-bit
AIFF is named `pcm_s8` by ffprobe and `pcm_u8` here.

## Fuzzing

Six targets in `tests/fuzz/` (open, packets, probe, audio, video, resample), built with UBSan (plus float-cast checks), hardened libc++ and
coverage instrumentation. The compiler on the test machine ships no libFuzzer runtime, so `tests/fuzz/driver.cpp` is a small
coverage-guided engine with the same entry point and option names; `build.sh` uses the compiler's own engine when it has one. The driver
saves the input on a crash, on a 25 s hang and when resident memory passes a limit. `GUARD=1` runs a target under guard malloc.

| Found | Where | Fix |
|---|---|---|
| Signed overflow in fragment times (undefined behaviour) | `src/mp4_read.cpp`, and the same pattern in other timestamp sums | the library is compiled with `-fwrapv`: timestamp arithmetic on hostile values wraps instead of being undefined |
| Infinite bit rate converted to an integer (undefined behaviour) | `src/probe.cpp` | every float-to-integer conversion in the probe is clamped |
| A 60 KB fragmented MP4 made the reader allocate over 3 GB: `trun` boxes without per-sample fields may claim 16 million samples each | `src/mp4_read.cpp` | all fragments of a file together may not claim more samples than the file has bytes |
| The same for a fixed sample size with a huge sample count | `src/mp4_read.cpp` | the count is limited to file size / sample size |

The inputs are kept in `tests/fixtures/fuzz/` and replayed by `tests/suites/85_fuzz.sh`, which also pushes every committed fixture
through every target and runs a short burst of mutated inputs per target.

## One real project, imported and exported with no FFmpeg

`experiments/absentmotion/run_zero.sh` runs the desktop app's own import and export of one real project (13.5 s, 1080 x 1080, 60 fps,
three H.264 clips) with two stand-in programs in place of `ffmpeg` and `ffprobe`, a second engine library that decodes the clips with
FFamp, and every real FFmpeg program and library on the machine denied by a sandbox for the whole process tree.

| Check | Result |
|---|---|
| FFmpeg command lines the app runs for one import and export | 10 calls of 9 kinds; all answered by the stand-ins, none refused |
| Real FFmpeg programs and libraries tried from inside the sandbox | 36 of 36 blocked |
| Frames, length | 811 and 13.5167 s on both paths |
| Checksum of every rendered frame, FFmpeg decoder against FFamp | 811 of 811 identical |
| The app's audio mix (clip sound decoded by FFmpeg against FFamp) | same length to the sample (596,055); 99.6 % of sample values identical, largest difference 2 of 32,768, 110 dB |
| The clip's sound alone, with the app's two exact decode commands | same length to the sample, 136 dB |
| The app's own check of the exported file | passed |
| Export time, middle of three after a warm-up | 3,017 ms against 3,484 ms with FFmpeg (13 % less) |
| Exported file in ffprobe, in a full FFmpeg decode and in AVFoundation | no messages, all frames and samples read |

Limits: the stand-ins are separate programs because the app was not changed; they cover only the command lines of this H.264 + AAC MP4
export; one project on one machine.

# Speed round: sound paths, Vorbis, APV and outside judges

Whole-process wall time on the same Mac, ours and FFmpeg 9.0.2 run in turn, best of five.

| Path | Before | Now | FFmpeg |
|---|---|---|---|
| AAC file (87 s, 44.1 kHz stereo) decoded and resampled to 48 kHz, `pcm` | 0.145 s | 0.046 s | 0.066 s |
| Raw 44.1 kHz stereo (60 s) to 48 kHz through the streaming `Resampler`, large pushes | 0.116 s (busier machine) | 0.022 s | 0.046 s |
| The same in 1024-frame pushes (one thread) | 0.151 s (busier machine) | 0.049 s | 0.046 s |
| FLAC, 60 s stereo 16-bit | 0.584 s, 5,180,850 bytes | 0.064 s, 5,192,500 bytes | 0.094 s, 5,463,653 bytes |

- **Resampler**: the same 160-tap Kaiser filter and table as before (135 dB against the exact sweep, a tone above the new limit is left
  at -128 dB, no delay). The filter sum is written with NEON and shares the coefficients between the two channels of a pair, the phase
  is stepped instead of divided for every sample, a render of 8192 frames or more is split over the cores, and `PcmReader` reads ahead in
  65536-frame windows while the next window is decoded on another thread. Every output sample is still a pure function of its position,
  so any chunking, the threaded path and the streaming path give the same bits (checked in `tests/suites/75_installed.sh`).
  In small pushes on one thread it is level with FFmpeg's 32-tap default while doing five times the filter work.
- **FLAC encoder**: the stereo layout and the predictor order are chosen from the autocorrelation (one Levinson pass gives the expected
  size of every order up to 12), then only the best fixed and the best linear predictor are tried for real. The residual uses 32-bit
  sums when the block proves they cannot overflow, the bit writer works in words, and 32 blocks at a time are coded on all cores while
  the MD5 runs beside them. CPU time fell from 0.58 s to 0.16 s; the files are 0.2 % larger than the exhaustive search made them and
  5 % smaller than FFmpeg's. The stream is identical in every run and decodes to the input in our decoder, FFmpeg and the `flac` tool.
- **Vorbis** (libvorbis, `ABSENT_VORBIS`): `AudioDecoder` takes `vorb` tracks from Ogg, Matroska and WebM with random access. Against
  FFmpeg's decoder on files made by libvorbis: same length, no offset, 136 to 137 dB. Files made by FFmpeg's own experimental Vorbis
  encoder decode differently in FFmpeg and in libvorbis on the second channel; VLC agrees with libvorbis.
- **APV** (liboapv, `ABSENT_OAPV`): `EncodeSpec.codec = apv1` writes 4:2:2 10-bit APV (profile 422-10, average bitrate, BT.709
  limited), which is what the app's FFmpeg export ends up with, into MP4 or MOV with an `apvC` box. FFmpeg's APV decoder reads our files
  and gives the same bits as our own decoder (liboapv again); 65 dB luma PSNR at 20 Mbit/s for 480x270, about 700 frames a second.
- **Judges** in the suite, each skipped when the tool is missing: `flac -t` on every FLAC file in the output folder, `mkvmerge -J`
  (no error, no warning) on every Matroska and WebM file, VLC opening eight of our files with no window (`-I dummy`, dummy outputs;
  it always exits with 0, so the check reads its log for the demuxer, the decoders and error lines).
- **yt-dlp**: `docs/YTDLP.md` was run for real; it found that the transport stream reader lost one picture per HLS piece.

# Speed round: every video path that was slower than FFmpeg

The owner's 1080p, 30 fps, 30 second clip (901 frames) in each codec. Whole-program wall time, best of three, with the old build,
the new build and FFmpeg run one after another in the same minute (another job was running on the machine, so every figure is a
little slower than on an idle Mac, but all three saw the same load). "Before" is the build before this round.

| Path | Before | After | FFmpeg 9.0.2 |
|---|---|---|---|
| HEVC 10-bit decode into memory | 1.43 s | 0.39 s | 0.83 s (`-hwaccel videotoolbox`) |
| AV1 software decode into memory (dav1d) | 2.05 s | 0.64 s | 0.82 s (libdav1d) |
| VP9 software decode into memory (libvpx) | 1.21 s | 0.94 s | 0.68 s (FFmpeg's own VP9 decoder) |
| VP9 through the system decoder into memory | 1.24 s | 0.96 s | 0.76 s (software) |
| H.264 decode into memory | 0.78 s | 0.45 s | 1.74 s (`-hwaccel videotoolbox`) |
| HEVC 8-bit decode into memory | 0.55 s | 0.37 s | 1.50 s |
| AV1 through the system decoder into memory | 0.61 s | 0.38 s | 1.53 s |
| VP9 encode, 6 Mbit/s, from the H.264 clip | 4.67 s | 4.00 s | 4.29 s (libvpx-vp9, realtime, cpu-used 8, row-mt) |
| AV1 encode, 6 Mbit/s, from the H.264 clip | 3.46 s | 3.38 s | 3.46 s (libsvtav1 preset 10) |
| H.264 / HEVC encode, 6 Mbit/s (hardware) | 4.05 / 4.20 s | 4.05 / 4.26 s | 4.09 / 4.30 s |
| Frames out, 1080p to 720p RGBA, bicubic | 4.36 s (207 fps) | 0.95 s (950 fps) | 1.20 s (`flags=bicubic`), 1.62 s (`accurate_rnd+full_chroma_int+bicubic`, the same quality) |
| Frames out, 1080p RGBA, no scaling | 3.47 s | 0.56 s | 0.74 s |
| Frames out, 1080p to 720p I420, bicubic | 2.62 s | 0.66 s | 0.71 s |
| Compress 60 s of 1080p to 720p, 4 Mbit/s, AAC | 4.17 s | 4.13 s | 4.24 s |
| Jump to a random frame, mean of 20: H.264 / HEVC / HEVC 10-bit / AV1 (system decoder) | 24.1 / 20.4 / 21.4 / 34.8 ms | 24.1 / 20.4 / 20.6 / 34.8 ms | 76 to 141 ms (one program start per jump) |
| Jump to a random frame in software: AV1 / VP9 | 187 / 72 ms | 66 / 62 ms | |

What changed:

- The decoder reads ahead when frames are taken in order: up to 8 packets (4 above 2304 x 1296) are already in the hardware decoder,
  or in a worker thread for dav1d and libvpx, while the caller copies or converts the current frame. A jump to another frame does not
  read ahead, so random seeks cost what they did before. After a run of frames, a jump first waits for the frames in flight.
- dav1d decodes several frames at once (frame delay 8, or 4 for large pictures) and is drained at the end of the stream and after a
  jump. libvpx filters with all its threads. Peak memory for 1080p AV1 software decode went from 54 MB to about 190 MB.
- The 10-bit and interleaved plane copies are plain loops the compiler turns into NEON code.
- Scaling and colour conversion run as one pass over bands of rows that fit the cache: the three planes are scaled and converted to
  RGBA inside the band, straight from the decoder's planes, on a thread crew that lives as long as the program instead of new
  threads for every frame. The bicubic weights and the arithmetic are the same as before.
- SVT-AV1 gets `under_shoot_pct` 5.

Exactness: `tests/suites/89_speedcheck.sh` reads runs of frames after jumps in 12 fixture cases and compares them with the straight
decode; the frame-exact checks against FFmpeg in the video suites are unchanged and pass. Scaled and converted frames were compared
with the build before this round in twenty cases (ten sizes and layouts, an 8-bit and a 10-bit clip): seventeen byte-identical,
three with 1 to 75 bytes out of 16 to 43 million differing by at most 2 (a value that lands exactly on a rounding tie). Against FFmpeg's bicubic scaling
(`accurate_rnd+full_chroma_int`) the 720p RGBA frames measure 74.7 dB on the owner's clip before and after, 53.4 dB on the suite's
moving test clip.

Not faster than FFmpeg:

- VP9 software decode: 0.94 s against 0.68 s. libvpx decodes one frame at a time and spreads only tiles and the loop filter over
  threads; FFmpeg's own decoder works on several frames at once and needs a quarter less processor time. Closing the gap needs a
  second decoder instance running ahead in the next group of pictures (about 130 MB more memory at 1080p for, at best, a tie) or a
  frame-threaded VP9 decoder. Not done.
- Compress and the hardware encodes: the hardware encoder sets the pace (446 fps for 720p H.264 when fed from memory), ours and
  FFmpeg's times are within 3 %. The 5.6 to 6.8 s measured earlier was a single run on a busy machine.

AV1 size at a 6 Mbit/s target (22.5 MB for this clip):

| Key frame every | Ours before | Ours after | FFmpeg libsvtav1, preset 10 |
|---|---|---|---|
| 30 frames (what our tools and the app ask for) | 14.35 MB, 43.89 dB | 16.77 MB, 44.44 dB | 14.60 MB, 43.91 dB (`-g 30`) |
| 150 frames (the encoder's own choice, FFmpeg's default) | 18.95 MB, 45.14 dB | 21.21 MB, 45.49 dB | 18.47 MB, 44.93 dB |

The earlier comparison set our 30-frame groups against FFmpeg's 150-frame default; with the same spacing the two were equal. The
one-pass rate control of SVT-AV1 falls short of the target with short groups, and it cannot be steered while it runs (a bitrate
change is only accepted in its low-delay constant-rate mode). On two other clips the new setting gave 77 % and 100 % of the target
with 30-frame groups (66 % and 100 % before) and 95 % and 102 % with 150-frame groups. `ffamp transcode` takes `--gop`.

# Formats round: what the Mac app still needed FFmpeg for

Suite `tests/suites/45_formats.sh`, fixtures in `tests/fixtures/formats` (made by `make.sh` with FFmpeg, afconvert and, for AMR,
opencore-amr and vo-amrwbenc).

| Input | Opens through | Against FFmpeg's decode |
|---|---|---|
| AC-3, E-AC-3 in .ac3/.eac3, MP4/MOV, Matroska, CAF, AVI, transport streams | AudioToolbox | 50 to 78 dB; length equal to the sample |
| AMR-NB, AMR-WB in .amr, 3GP/MP4, CAF | AudioToolbox | 32 to 35 dB (two different decoders of a speech codec) |
| CAF: PCM, AAC, HE-AAC, ALAC, FLAC, Opus (1-2 channels), MP3, AC-3, AMR | own reader | PCM, ALAC, FLAC bit-exact; AAC, Opus 135 dB; MP3 88 dB |
| AVI: H.264 | VideoToolbox | frame-exact, B-frames included |
| MPEG-4 part 2 Simple Profile in MP4/MOV, Matroska, AVI | VideoToolbox | 49 dB or more |
| MPEG-1, MPEG-2 in MP4/MOV, Matroska, AVI | VideoToolbox | 52 dB or more |
| Motion JPEG 4:2:0 | VideoToolbox (hardware) | 67 dB or more |
| Motion JPEG 4:2:2, 4:4:4 | ImageIO | 31 dB (4:2:2, colour filled in by different filters) and 52 dB (4:4:4), compared as BGRA |
| DV PAL and NTSC in MOV, Matroska, AVI | VideoToolbox | 36 to 37 dB (Apple's DV decoder differs this much from FFmpeg's) |
| H.263 in 3GP | VideoToolbox | 62 dB or more |
| AVI over 1 GB (OpenDML, several RIFF parts; 2.2 GB tried by hand) | own reader | frame count equal, frames from start to end 65 dB |

Not possible on a Mac without writing or bundling a codec:

- **WMA / WMV / ASF**: AudioToolbox has no WMA decoder (`AudioFileOpenURL` fails, `afconvert` lists no such format) and AVFoundation
  answers "Cannot Open". `Reader::open` said `Windows Media files (WMA, WMV, ASF) cannot be opened: this system has no decoder for them`
  until the ASF reader of the Windows Media round.
- **MPEG-4 Advanced Simple Profile** (most DivX and Xvid files): VideoToolbox and AVFoundation refuse B-frames, quarter-pixel motion
  and custom matrices. The file opens, probes, and its sound decodes; the video decoder fails with its own message.

Not built: MPEG-2 in transport streams and MPEG program streams (.mpg, .vob); neither is on the app's list.


# DivX and Xvid round: MPEG-4 Advanced Simple Profile through Xvid's decoder

xvidcore 1.3.7 (GPL-2.0-or-later) is an optional library like the others (`ABSENT_XVID`); `third_party/build_mac.sh` builds it static
for macOS 11 (C only on both architectures: `libxvidcore.a` is 673,400 bytes for arm64 and 778,904 bytes for x86_64). With it every
MPEG-4 part 2 track, Simple Profile included, is decoded by `src/opt/xvid_decode.cpp` (backend `xvid`); how it is driven is in
`docs/CONVENTIONS.md`. Without it the system decoder plays Simple Profile and Advanced Simple Profile is refused as before.

Suite `tests/suites/46_mpeg4.sh`, fixtures in `tests/fixtures/asp` (made by `make.sh`: FFmpeg's encoder for the `lavc_` files,
Xvid's own encoder through `xvid_encode.c` for GMC and the `xvid_` files, because FFmpeg cannot write S-VOPs). Every file has
30 frames, 160x120 or 176x144.

| File | Tools | Against FFmpeg 9.0.2 |
|---|---|---|
| `lavc_qpel.mp4`, `.mkv`, `.avi` | 2 B-frames, quarter-pixel, 4 vectors, `-mbd 2`, trellis | bit-exact with `-idct xvid`; 63.8 dB with FFmpeg's own IDCT |
| `lavc_packed.avi` | the same stream as a DivX packed AVI (`DivX503b1393p`) | same pictures and frame times as the MP4; bit-exact against `-bug 0 -idct xvid` |
| `lavc_matrix.mkv` | MPEG quantiser with custom intra and inter matrices, B-frames | bit-exact with `-idct xvid`; 62.8 dB |
| `lavc_interlaced.mp4` | field DCT, alternate scan, quarter-pixel, B-frames | bit-exact with `-idct xvid`; 64.0 dB |
| `lavc_fields.mkv` | field DCT and field motion vectors (P-frames) | bit-exact with `-idct xvid`; 60.4 dB |
| `xvid_gmc.avi` | Xvid packed AVI: GMC S-VOPs, quarter-pixel, 2 B-frames | 26 of 30 frames bit-exact, the others 85.7 dB (one level apart on 2 to 5 pixels of one S-VOP and the B-frames that use it) |
| `xvid_interlaced.avi` | Xvid packed AVI: interlaced, custom matrices, 2 B-frames | bit-exact |

FFmpeg picks Xvid's IDCT by itself for streams with Xvid's user data; for its own streams it uses its simple IDCT, which is
where the 60 to 64 dB come from. The existing formats fixtures (Simple Profile in MP4, Matroska and AVI, B-frames in MP4 and AVI,
the hand-packed AVI) measure 61.3 and 63.1 dB against FFmpeg's simple IDCT and are bit-exact with `-idct xvid`.

Also checked: frame counts and decoded frame times against ffprobe (MP4 and Matroska), the same pictures and times from MP4,
Matroska, plain AVI and the packed AVI, random frames against the straight decode and against FFmpeg, ten threads decoding one file
at once (each gets the straight decode), no leaks after random jumps (`leaks --atExit`), damaged copies, and the new fuzz target
`fuzz_mpeg4` (the raw stream cut at each VOP, with jumps): 137,730 runs under guard malloc and the undefined-behaviour checks,
nothing found. The FFmpeg mode and the no-FFmpeg mode of the suite both pass, and a build without xvidcore still refuses Advanced
Simple Profile with its message.

Speed, 1280x720, best of three on this Mac: an FFmpeg-made ASP file (quarter-pixel, 2 B-frames, 2.5 Mbit/s, 600 frames) decodes into
memory at 1,167 frames a second (0.51 s; FFmpeg 0.24 s on one thread, 0.08 s with its frame threads); an Xvid-made packed AVI with
GMC and quarter-pixel (300 frames) at 714 frames a second (0.42 s; FFmpeg 0.33 s, which cannot thread a packed stream either).
Xvid's decoder is plain C on both architectures and runs on one thread; that is fast enough for DivX-sized video, so nothing more
was done.

The AVI reader now treats a packed N-VOP that follows no packed pair (Xvid's encoder writes one when it is flushed with B-frames
waiting) as a slot with no picture, as FFmpeg does, instead of a frame that repeats the one before.

Still not decoded right, because Xvid's decoder does not do it:

- B-frames with field motion vectors (FFmpeg's `-flags +ilme` with B-frames): Xvid reads one vector per direction where the stream
  has two, so those B-frames come out broken (18.6 dB). Xvid's own encoder never writes them.
- Field motion vectors together with quarter-pixel motion (FFmpeg's `+ilme+qpel`): the P-frames differ from FFmpeg's decode.
- DivX 5 files made with quarter-pixel motion by builds before 1814, and Xvid files from before Xvid wrote its user data: FFmpeg
  imitates those encoders' rounding bugs, Xvid decodes them as correct streams.

# Windows Media round 1: the ASF reader

`src/asf_read.cpp` opens .wma, .wmv and .asf files: every audio and video stream, its packets, times, key frames and tags (rules in
`docs/CONVENTIONS.md`). Sources: the expired ASF patent US 6,041,345, the file-analysis description of ASF at
avifile.sourceforge.net, and what real files show, checked packet by packet against FFmpeg's demuxer. Microsoft's ASF specification
1.20.03 was not used: its licence allows implementations in object code only, rules out open-source licences and ended on
1 January 2012. No code of another implementation was read for the reader.

Suite `tests/suites/47_asf.sh`, fixtures in `tests/fixtures/asf` (10 files, 560 KB, made by `make.sh` with FFmpeg; `craft.py` makes
altered copies at test time). Sample files that may not be committed are tested when `WINMEDIA_SAMPLES` names a folder of them.

| Check | Result |
|---|---|
| Every packet of the 10 fixtures and 2 altered copies (stated packet lengths; scrambled sound) against `ffprobe -show_packets -show_data_hash` | bytes, times and key flags all equal; MP3 and PCM, which are cut into their own frames, equal as byte streams with the same start |
| 29 Microsoft-made and FATE files: WMV7, WMV8, WMV9 Simple, Main and Complex, VC-1 Advanced (also interlaced), WMVA, Microsoft MPEG-4 v3, WMA 1, 2, Pro, Lossless and Voice, up to 6,699 pictures and 1080p | every packet equal; on four cut-off files FFmpeg also passes on the cut part of the last media object (1 or 2 packets more) |
| Probe fields against ffprobe | codec names, profiles, sizes, pixel and sample formats, rates, channels, time bases, frame counts and tags equal (differences kept on purpose below) |
| H.264 with B-frames in ASF | frame-exact against FFmpeg's decode; random frames equal the straight decode |
| MPEG-4 part 2, H.263 and Motion JPEG in ASF (system decoder) | 49, 62 and 68 dB |
| MP3 and PCM in ASF | 71 to 77 dB; PCM bit-exact |
| Scrambled sound (audio spread, span 4, made by `craft.py`) | put back bit-exact; FFmpeg's demuxer agrees |
| Key frames from the index when the payloads carry no key bits | FFmpeg-made files: all found; five Microsoft-made files: 8 of 8, 9 of 9, 16 of 17 and 38 of 40 with none wrong, 281 of 565 with 3 wrong (the index lists one picture a second) |
| Seeking with `ffmpeg -ss` as judge | FFmpeg always starts on one of our key frames, at or before ours |
| DRM | a real DRM-protected file (FATE `wmv_drm.wmv`) and two altered fixtures are refused with the DRM message |
| WMA, WMV and Microsoft MPEG-4 in ASF and AVI | probe names them; the decoders say `this video codec (...) is not decoded yet` |
| Fuzzing with undefined-behaviour checks, ASF seeds, packets, probe, audio and video targets | over a million runs; one find (below) |
| Open and probe a 1 GB WMV (24,000 pictures, warm cache) | 0.10 s, 22 MB peak; ffprobe 0.025 s for its header-only answer, 0.18 s to list the packets |

What the tests found:

- FFmpeg writes H.264 into ASF with decode times and its demuxer gives no presentation times, so the display order comes from the
  picture order counts.
- FFmpeg's Simple Index gives a packet count that is not the number of packets the key frame spans (Microsoft's is); the entry's time
  picks the right media object in both.
- `asf_msmpeg4v2_mp3_asf2divX_fault.asf` (a sample from an FFmpeg bug report): FFmpeg stops after 7 pictures with invalid packet
  lengths; FFamp reads all 1,035 pictures and 69 s of MP3.
- Fuzzing: an H.264 parameter set with a huge picture order count length made a shift undefined (now bounded); the input is kept in
  `tests/fixtures/fuzz/open/`.

Differences from ffprobe kept on purpose: each stream's length is its own (ffprobe gives every stream the file's play duration and
adds it to the first time), bit rates are counted from the packets (ffprobe takes the stated ones), frame rates of streams without a
fixed rate (ffprobe 0/0 or 1000/1), language as stored (`en-us`, ffprobe `eng`), and ffprobe starts the H.264 track of an
FFmpeg-made file at the first sound packet (0.057 s).

Test counts: `47_asf.sh` 79 checks (61 without FFmpeg) and 69 more on the sample files; `QUICK=1 tests/run.sh` 2,276 pass, 1 skipped (a real camera); `QUICK=1 NOFFMPEG=1 tests/run.sh` 1,479 pass, 89 skipped.

Not done: the Windows Media decoders themselves (plan and order in the private notes); Media Object Index and Timecode Index objects;
payload extension data past the standard 8 replicated bytes; Stream Bitrate Properties; mutual exclusion (every stream of a
multi-bitrate file is listed); ISO 639-2 language codes.


# Windows Media step B: VC-1 (WMV3, WVC1, WMVA)

FFamp's own VC-1 decoder (`src/vc1_*.cpp`; backend `vc1`), based on FFmpeg's VC-1 decoder as the owner's rule of
2026-11-13 allows: the FFmpeg C was turned into FFamp's C++ (one `Vc1` context in place of `VC1Context` and `MpegEncContext`, member
functions, no comments), the tables were copied by script from FFmpeg's data headers, and every derived file is listed in
`docs/NOTICE.md`. How it is driven is in `docs/CONVENTIONS.md`.

FFmpeg cannot encode VC-1, so the committed fixtures are written by `tests/fixtures/vc1/make.py`, a small WMV3 writer of FFamp's own:
Main profile with B-pictures (direct and interpolated), intensity compensation, range reduction, overlap smoothing and the loop
filter, and Simple profile with bilinear, half-pel and quarter-pel motion (`main_bframes.avi` 1.7 KB, `simple.avi` 1.2 KB). The
public sample files of the plan's appendix A are tested when `WINMEDIA_SAMPLES` names their folder; they are not committed.

| File | What it is | Result against FFmpeg's decode |
|---|---|---|
| `main_bframes.avi`, `simple.avi` (fixtures) | Main with B, IC, range reduction; Simple | every frame bit-exact, in order |
| `sample-wmv9-simple-SP@ML.wmv`, `sb-spl-q94.avi` | WMV3 Simple, CIF and 640x480 | 595 and 193 frames bit-exact |
| `sample-wmv9-main-MP@LL.wmv`, `sb-main-q94.avi` | WMV3 Main | 595 and 193 frames bit-exact |
| `sb-adv-q94.avi` | WMV3 Complex without X8 (simple IDCT) | 193 frames bit-exact |
| `Robotica_720.wmv`, `Robotica_1080.wmv` | WMV3 Main 1280x720 and 1440x1080 | 480 and 480 frames bit-exact |
| `480i30__codec_WVC1__...wmv` | WVC1 Advanced, interlaced field pictures, B | 66 frames bit-exact |
| `wmva-smpl-bdsm.wmv` | WMVA (old Advanced profile framing), B | 1,004 frames bit-exact; FFmpeg shows one more picture from the media object the end of the file cuts off |
| FATE `SA00040`, `SA00050`, `SA10091`, `SA20021` | Advanced, progressive, B, slices | every picture bit-exact |
| FATE `SA10143`, `ilaced_twomv` | Advanced, interlaced field and frame pictures, 2-MV | every picture bit-exact |
| FATE `SMM0005.rcv`, `SMM0015.rcv` | WMV3 Main with and without B | every picture bit-exact; also in display order from an AVI |

The raw `.vc1` and `.rcv` files have no display times, so FFmpeg's Matroska copy of them is compared picture by picture without the
order. Random frames (12 to 16 per file, in random order) equal the straight decode and FFmpeg's frame at that time on every file.

Where FFmpeg is wrong or silent (plan section 3.6), ST 421 decides:

- **Range mapping** (Advanced profile `RANGE_MAPY`, `RANGE_MAPUV` in the entry point): FFmpeg reads the values and ignores them.
  FFamp applies ST 421 6.2.15 and 6.2.16 to the output pictures only (the references stay unmapped, ST 421 5.6). Test: the
  interlaced WVC1 sample with its entry point patched to `RANGE_MAPY` 2 and `RANGE_MAPUV` 5 gives FFmpeg's pictures mapped by the
  formula, every pixel of 66 frames.
- **Pan-scan** (`PANSCAN_FLAG`): FFmpeg does not read the pan-scan windows and loses its place in the picture header. FFamp skips
  them as ST 421 8.9.1 counts them (no sample file has them).
- **Progressive segmented frames** (`PSF`): FFmpeg refuses the stream; the flag only changes how frames are shown, so FFamp decodes
  them as progressive frames (no sample file has it).
- Not changed (FFmpeg's behaviour kept, no sample to check against): direct mode in a B field picture whose anchor is a frame picture
  (an error, as in FFmpeg), the chroma motion vector pull-back FFmpeg marks as a FIXME, old interlaced mode (`res_y411`, refused),
  multiresolution pictures (`RESPIC` read, not scaled), and X8 intra pictures of the Complex profile (refused with a message; the
  plan puts X8 in step D).

Test counts (milestones 1 to 4): `QUICK=1 tests/run.sh` 2,376 pass, 1 skipped (a real camera); `QUICK=1 NOFFMPEG=1 tests/run.sh`
1,544 pass; with `WINMEDIA_SAMPLES` set, `48_vc1.sh` alone gives 74 checks (43 without FFmpeg), all pass.
# Windows Media round 2: Windows Media Audio 1 and 2, and the audio framework for the family

`src/wma_decode.cpp` decodes Windows Media Audio 1 (`0x0160`) and 2 (`0x0161`) from ASF, AVI and Matroska behind `AudioDecoder`. It is
based on FFmpeg's decoder (files and holders in `docs/NOTICE.md`), rewritten in FFamp's style on a small framework the later WMA
rounds share: a bit reader, multi-level VLC tables, an inverse MDCT of any power-of-two size with sine windows, the WMA run-level
coder and tables, and four additions to `PacketDecoder` (exact frame counts, a hidden start, state marks, parse-only skimming). Rules
in `docs/CONVENTIONS.md`.

Suite `tests/suites/48_wma.sh`, fixtures in `tests/fixtures/wma` (7 files, 63 KB, made by `make.sh` with FFmpeg) plus the three WMA
files of `tests/fixtures/asf`. Judge: FFmpeg 9.0.2's `wmav1`/`wmav2` decoders, float output.

| Check | Result |
|---|---|
| 10 fixtures: WMA 1 and 2, 11,025 to 48,000 Hz, noise coding, mid/side, in ASF, AVI and Matroska | 137.3 to 139.7 dB; lengths equal to the sample |
| 19 encoder settings made at test time: both versions, 8,000 to 48,000 Hz (12,000 and 24,000 too), mono and stereo, 24 to 320 kbit/s, noise coding on and off, all three coefficient tables, frames of 512, 1,024 and 2,048 | 137.0 to 138.5 dB; lengths equal |
| Matroska and AVI written by FFmpeg directly (Matroska with its CodecDelay start skip) | 137.3 and 137.7 dB; lengths equal |
| 7 Microsoft-made files (WMA 1 at 44.1 kHz; WMA 2 at 8, 16, 22.05, 44.1 and 48 kHz with bit reservoir, variable block sizes, LSP exponents and noise coding; up to 280 s) | 137.2 to 138.5 dB, a silent one bit-exact; lengths equal |
| Random reads (single frames, odd chunk sizes, reads before the start and past the end) on every file | identical to the straight decode, noise-coded files included |
| One stream in ASF, AVI and Matroska | identical samples |
| Fuzzing with undefined-behaviour checks: `fuzz_wma` (format header and packets, with resets, marks and skims) and `fuzz_audio` on WMA files | 16,277 and 117,353 runs; one find (below) |

Why not bit-exact: FFmpeg's inverse MDCT runs its own FFT (NEON on this Mac) and FFamp a portable radix-2 one, so float rounding
differs: the largest difference is 1.5e-7 to 3.6e-7 of full scale (about one step of 24-bit sound), everything else follows
FFmpeg's arithmetic operation for operation (the same float and double steps in the exponent, noise and window code).

Timing: FFmpeg hides the decoder's first two frames (its `delay`) and FFamp plays them as silence, so every later sample is the same
and stays at the same time; a Matroska CodecDelay replaces the two frames in both. Packets are placed by the frame counts in the
superframe headers, not by the container's millisecond times: in the Microsoft-made files the packet times do not always match the
counts (the first packet is one frame short in three files, the times wander by a few milliseconds in another), while FFmpeg's
decoded samples, like FFamp's, follow the counts.

Seeking: jumps restart three packets early and need no state for streams without noise coding. Noise coding draws from a generator
whose position depends on every block before, so FFmpeg's own seek gives different noise than its straight decode; FFamp keeps a mark
per 20-second run and parses forward from the nearest one (no transforms): a first jump to the end of a 20-minute noise-coded file
takes 0.06 s, later jumps nothing extra. Opening a bit-reservoir stream reads each packet once for the frame counts (0.01 s for the
138-second `kelsy_300.wmv`).

What the tests found:

- FFmpeg's `wmav1`/`wmav2` decoders skip two frames at the start that its encoder did not add (the encoder primes one frame), so
  FFmpeg's own round trip loses one frame of sound; FFamp keeps the samples FFmpeg shows and plays the two frames as silence.
- In FFmpeg's decoder a superframe that ends no frame adds eight times its length to the reservoir length (bits where bytes are
  meant); FFamp adds bytes. No sample file has such a superframe.
- Fuzzing: a huge average byte rate in the format header overflowed a float-to-int conversion (FFmpeg has the same); now refused.

Speed, best of three, whole files through the command line (`ffamp adecode` against `ffmpeg -threads 1 ... -f f32le`):

| File | FFamp | FFmpeg, one thread | FFamp / FFmpeg |
|---|---|---|---|
| WMA 2, 128 kbit/s stereo, 44.1 kHz, 600 s (tone and pink noise) | 0.277 s (2,170 times real time) | 0.264 s | 1.05 |
| WMA 2, 32 kbit/s stereo, 44.1 kHz, noise coding, 600 s | 0.234 s | 0.217 s | 1.08 |
| WMA 1, 48 kbit/s stereo, 22.05 kHz, 600 s | 0.131 s | 0.154 s | 0.85 |
| `nintendothemesacappellax.wmv`: WMA 2, 16 kbit/s mono, 16 kHz, reservoir, LSP, noise coding, 280 s | 0.036 s | 0.051 s | 0.71 |
| `kelsy_300.wmv`: WMA 2, 64 kbit/s stereo, 48 kHz, 139 s (the video is not decoded) | 0.065 s | 0.063 s | 1.03 |
| `wandue_project_-_king_of_my_castle.asf`: WMA 1, 64 kbit/s stereo, 44.1 kHz, 101 s | 0.045 s | 0.058 s | 0.78 |

Both columns include starting the program, reading the file and writing the samples to a pipe. The suite checks the 128 and 32 kbit/s
cases (180 s with `QUICK=1`) and the three sample files against a limit of 1.5.

Test counts: `48_wma.sh` 84 checks with FFmpeg (17 more on the sample files), 27 without; `QUICK=1 tests/run.sh` 2,435 pass, 1 skipped
(a real camera); `QUICK=1 NOFFMPEG=1 tests/run.sh` 1,550 pass, 91 skipped.

Not done: WMA Pro, Lossless and Voice (steps C, F and G); the AVI stream header's time base and length in probe (FFamp gives the
length of the data and a sample-rate time base); reading only the superframe headers at open (it reads whole packets, which costs
more on very large WMV files with a cold cache).


# Windows Media round 3: Windows Media Audio 9 Professional

`src/wmapro_decode.cpp` decodes Windows Media Audio 9 Professional (`0x0162`) from ASF, AVI and Matroska behind `AudioDecoder`, on the
framework of round 2 (bit reader, VLC tables, inverse MDCT, sine windows, the run-level coder). It is based on FFmpeg's decoder
(`docs/NOTICE.md`), rewritten in FFamp's style. Rules in `docs/CONVENTIONS.md`.

FFmpeg cannot encode this codec, so the tests use two kinds of input: the Microsoft-made and FATE sample files (not committed, run when
`WINMEDIA_SAMPLES` names their folder), and 10 small fixtures in `tests/fixtures/wmapro` (200 KB) written by `make.py`, a bitstream
writer of FFamp's own that reads the code tables from the decoder's source and covers what no public sample has: 1, 2, 3, 4, 5, 6, 7
and 8 channels (masks mono, stereo, 3.0, quad, 5.0, 5.1(side), 6.1, 7.1 and none), 16, 20 and 24 bits, 32 to 96 kHz, frames of 2,048
and 4,096 samples, up to 16 sub-frames with a different layout per channel, M/S pairs, default and coded rotation matrices, per-band
transform switches, a silent LFE group, scale factors resampled and run-level coded, vector, escaped and run-level coefficients, large
values up to 24 bits, fill bits, escaped quantiser steps, dynamic range bytes, start and end trims, frames split across packets, in
ASF (remuxed by FFmpeg), AVI and Matroska. Judge: FFmpeg 9.0.2's `wmapro` decoder, float output.

| Check | Result |
|---|---|
| 10 fixtures, 1 to 8 channels | 134.9 to 138.4 dB; lengths equal to the sample; checksums for the runs without FFmpeg |
| `Classical_44_16_6_256000_0_20.wma` (5.1, 44.1 kHz, 256 kbit/s, frames without length fields, 190 s) | 137.4 dB, lengths equal |
| `Classical_96_24_2_Q75_2_3.wma` (stereo, 96 kHz, 24 bits, quality-based, 1,024-sample frames, 190 s) | 138.3 dB |
| `Robotica_720.wmv`, `Robotica_1080.wmv` (5.1, 48 kHz, inside WMV9 video files) | 137.4 dB |
| FATE `Beethovens_9th-1_small.wma`, `latin_192_mulitchannel_cut.wma` (stereo; 5.1 cut off) | 137.1 and 138.7 dB |
| `classical_22_16_2_16000_v3c_0_exclusive_0_29.wma` (22.05 kHz, 16 kbit/s) | FFmpeg and FFamp both give silence (bit-exact, no error); probably a low-bit-rate mode FFmpeg does not know (not confirmed) |
| Random reads (single frames, odd chunk sizes, reads before the start and past the end) on every file | identical to the straight decode |
| 5.1 and 7.1 mixed to stereo and mono by `ChannelMix` | 144 dB against the expected levels |
| Probe | `wmapro`, layout named from the mask (`quad`, `5.1(side)`, `7.1`), other fields as for the other ASF streams |

Why not bit-exact: as in round 2, the FFT under the inverse MDCT differs from FFmpeg's; the largest difference is 4.5e-8 to 3.6e-7 of
full scale. Accelerate's FFT was tried: it gives the same speed here, 2 dB less and different results from run to run (its code path
depends on where the buffers lie), so the portable FFT stays.

Timing: FFmpeg drops the first decoded frame; FFamp decodes it and plays it as silence, so every later sample is at the same place.
A start trim in that first frame is spent on the dropped frame in FFmpeg and FFamp alike; later trims cut their frame in both.

Frame counts and seeking: `AudioDecoder` needs the exact number of samples each packet gives. Frames without length fields can only be
found by parsing their coefficients, so `counts` parses the whole stream once without the transforms. A frame's parse does not depend
on earlier frames, so the packets are split into runs parsed on all cores, each run started one packet early; this takes 0.013 s for
the 190-second 5.1 file (0.07 s on one core). Jumps restart a few packets early (enough to cover two frames plus the largest packet)
and need no state marks; all random reads equal the straight decode.

What the tests found:

- FFmpeg's LFE cut-off (`subwoofer_cutoffs`) has no effect: the coefficients above it are cleared and then overwritten by the
  inverse quantisation. FFamp leaves it out, with the same output.
- The FATE `latin_192` file has a silent channel, which fooled the test's lag search (it reported a shift of -4,000); the check now
  relies on the SNR at zero shift.

Speed, best of three, whole files through the command line (`ffamp adecode` against `ffmpeg -threads 1 ... -f f32le`):

| File | FFamp | FFmpeg, one thread | FFamp / FFmpeg |
|---|---|---|---|
| `Classical_44_16_6_256000_0_20.wma`, 5.1, 190 s | 0.238 s | 0.210 s | 1.14 |
| `Classical_96_24_2_Q75_2_3.wma`, 96 kHz 24 bits, 190 s | 0.161 s | 0.185 s | 0.87 |
| `Robotica_1080.wmv`, 5.1, 21 s | 0.042 s | 0.053 s | 0.79 |

FFamp's whole-file time includes the counting parse (on all cores) and the parse inside the decode; FFmpeg parses once. The suite
checks these three files against the 1.5 limit when the samples are there.

Speed, whole program, best of three, decoding into memory (`ffamp bench --mode copy`; FFmpeg 9.0.2 `-f null`, which has no thread
support for VC-1 and gives the same time with `-threads 1`), measured while another build ran on the same Mac:

| File | Frames | FFamp | FFmpeg |
|---|---|---|---|
| `Robotica_1080.wmv`, 1440x1080 Main, P only | 480 | 536 fps | 576 fps |
| `Robotica_720.wmv`, 1280x720 Main, P only | 480 | 812 fps | 833 fps |
| `sample-wmv9-main-MP@LL.wmv`, 352x288 Main | 595 | 3,864 fps | 4,044 fps |
| `ilaced_twomv`, 1920x1080 interlaced fields, 8 of 13 pictures B | 13 | 151 fps | 102 fps |
| `SA10143`, 720x480 interlaced fields, half of the pictures B | 30 | 882 fps | 506 fps |
| `480i30__codec_WVC1__...wmv`, 720x480 interlaced, B | 66 | 1,082 fps | 853 fps |

The plan asked for at least 230 fps at 1440x1080; one core gives 536 (suite check: over 230). Since the merge with the WMA round
the decoder reads its bits through the shared `BitReader` and `Vlc` (`src/bits.h`, `src/vlc.h`) of the audio framework; its own
copies are gone, the pictures and the speed are unchanged. Most of the time goes to reading the
coefficients (about 30 %), motion compensation and the inverse transforms. The bicubic motion compensation is written with the
filter modes as template arguments so the compiler vectorises it; the chroma motion compensation uses NEON. With B-pictures on two
worker threads (milestone 5) the interlaced files run 1.5 to 1.7 times faster than one thread and faster than FFmpeg; streams with
no B-pictures (most WMV3) have nothing to overlap, because each P-picture needs the one before it.

Seeking: a jump starts at the key frame at or before the frame asked for (from the container's key flags, or from the picture types
in AVI) and decodes forward; every random frame tested equals the straight decode and FFmpeg's frame (16 per sample file, 10 per
fixture, and runs of frames after jumps on two sample files).

Fuzzing: the `vc1` target (the decoder fed raw sequence headers and pictures, seeded with `tests/fixtures/fuzz/vc1/`) ran 253,000
inputs with undefined-behaviour checks without a find; 560 damaged copies of seven sample files under Guard Malloc (heap overruns
and underruns trap) decoded or failed cleanly. AddressSanitizer hangs at start-up on this macOS, so it could not be used.


Test counts after milestone 5 and the merge with the WMA round: `QUICK=1 tests/run.sh` 2,458 pass, 1 skipped (a real camera);
`QUICK=1 NOFFMPEG=1 tests/run.sh` 1,569 pass; with `WINMEDIA_SAMPLES` set, `48_vc1.sh` alone gives 75 checks (44 without FFmpeg),
all pass.

Not done: X8 intra pictures (Complex profile, plan step D), the mixed field/frame direct mode and the chroma pull-back FIXME of FFmpeg
(no sample to check ST 421 against), old interlaced mode, multiresolution scaling, sprites, readers for raw `.vc1` and `.rcv` files,
threads for streams without B-pictures (each P-picture needs the one before; slices are rare), hand-written NEON for the transforms
and the loop filter.


# Windows Media round 4: Windows Media Audio 9 Lossless

`src/wmalossless_decode.cpp` decodes Windows Media Audio 9 Lossless (`0x0163`) from ASF, AVI and Matroska behind `AudioDecoder`,
based on FFmpeg's decoder (`docs/NOTICE.md`). It shares the packet and bit-reservoir handling of the 9 Professional decoder (each
codec keeps its own copy, as FFmpeg's do, because the two differ in small ways: buffer sizes, the drain at the end, when a split
frame is decoded). Rules in `docs/CONVENTIONS.md`.

FFmpeg cannot encode this codec either. Fixtures: 5 files in `tests/fixtures/wmalossless` (270 KB) written by `make.py`, FFamp's own
bitstream writer: 16 and 24 bits, mono, stereo and 5.1, raw PCM tiles with padding bits, one to three cascaded LMS filters with sent
coefficients, the multichannel LMS, the stereo decorrelation, the AC filter, a quantiser step, a silent channel, sub-frames, frames
with and without length fields split across packets, start and end trims, seekable tiles every 3 or 4 frames, in ASF (remuxed by
FFmpeg), AVI and Matroska. Judge: FFmpeg 9.0.2's `wmalossless` decoder.

| Check | Result |
|---|---|
| 5 fixtures | bit-exact, lengths equal; checksums for the runs without FFmpeg |
| `luckynight.wma` (stereo 16-bit, 60 s; FFmpeg's output equals the FLAC of the same recording) | bit-exact |
| `wma_0x163.wma` (stereo 16-bit, 98 s) | bit-exact |
| FATE `Mega_Weird_Audio_Test_24bit.wma`, `g2_24bit.wma` (24-bit, cut off) | bit-exact but the last frame, which reads past the cut (`docs/CONVENTIONS.md`); lengths equal |
| Random reads (single frames, odd chunk sizes, reads before the start and past the end) | identical to the straight decode on every file |
| Probe | `wmalossless`, `s16p` or `s32p`, layout from the mask |

Seeking: the prediction filters run on from one seekable tile to the next, so the decoder keeps marks of its whole state (42 KB for
`luckynight.wma`) and `AudioDecoder` decodes forward from the nearest one; reading straight through crosses each 20-second run with a
mark taken on the way, so it costs nothing extra.

Speed, best of three, whole files through the command line (`ffamp adecode` against `ffmpeg -threads 1 ... -f f32le`; FFmpeg's arm64
build has no NEON for this codec):

| File | FFamp | FFmpeg, one thread | FFamp / FFmpeg |
|---|---|---|---|
| `luckynight.wma`, 60 s | 0.284 s | 0.345 s | 0.82 |
| `wma_0x163.wma`, 98 s | 0.456 s | 0.542 s | 0.84 |

What is left: arithmetic-coded tiles and the inverse LPC filter (FFmpeg has neither; no public sample uses them, and no specification
is public); bitstream splicing (read and ignored, as in FFmpeg).
