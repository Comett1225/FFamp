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
1,544 pass; with `WINMEDIA_SAMPLES` set, `49_vc1.sh` alone gives 74 checks (43 without FFmpeg), all pass.
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
`QUICK=1 NOFFMPEG=1 tests/run.sh` 1,569 pass; with `WINMEDIA_SAMPLES` set, `49_vc1.sh` alone gives 75 checks (44 without FFmpeg),
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

Test counts after rounds 3 and 4 (with main's VC-1 merged): `48_wma.sh` 144 checks with FFmpeg (188 with the sample files of
`WINMEDIA_SAMPLES`), 65 without; `QUICK=1 tests/run.sh` 2,514 pass, 2 skipped; `QUICK=1 NOFFMPEG=1 tests/run.sh` 1,603 pass, 92
skipped.

Not done: Windows Media Audio 9 Voice (step G).

# Windows Media step D: Microsoft MPEG-4 v1 to v3, WMV7, WMV8 and X8 intra pictures

FFamp's own decoder for the H.263 family of Microsoft codecs (`src/msmpeg4.cpp` on the shared core `src/h263.h`, `src/h263.cpp`;
backends `msmpeg4v1`, `msmpeg4v2`, `msmpeg4v3`, `wmv1`, `wmv2`) and for the X8 intra pictures of WMV8 and VC-1 Complex profile
(`src/intrax8.cpp`), based on FFmpeg's decoders as the owner's rule of 2026-11-13 allows (files and holders in `docs/NOTICE.md`; the
tables were copied by script from FFmpeg's data files). How they are driven is in `docs/CONVENTIONS.md`.

The core reuses what the VC-1 and WMA rounds built instead of copying it: bit reader, VLC tables, the frame pool (now
`vc1::FramePool`, used by both decoders), half-pel copy and average, edge emulation, the simple IDCT family and the Microsoft MPEG-4
tables VC-1 already had. Two things were needed for bit-exact pictures:

- FFmpeg on arm64 decodes these codecs with its NEON simple IDCT, and that one computes rows that hold only a DC value in full where
  the C version takes a shortcut (8 x DC); above a DC of 1,024 the two differ by one before the column pass. `neonSimpleIdctPut` and
  `neonSimpleIdctAdd` give the NEON results (NEON intrinsics on arm64, plain C++ elsewhere); VC-1 Complex profile keeps the C version,
  which FFmpeg's VC-1 decoder asks for by name.
- FFmpeg's v3 decoder uses the old DC scale tables, because its bug workarounds start in auto-detect mode; FFamp does the same.

Readers: AVI, ASF and Matroska now name every fourcc FFmpeg maps to these codecs (`DIV3`, `DIV4`, `DIV5`, `DIV6`, `MPG3`, `DVX3`,
`AP41`, `COL0`, `COL1` as `MP43`; `DIV2` as `MP42`; `MP41` as `MPG4`; `GXVE` as `WMV2`; Matroska `V_MPEG4/MS/V3`) and give them
their BITMAPINFOHEADER. `DIV5` and `DIV6` used to go to Xvid as MPEG-4 part 2. AVI takes their key frames from the picture headers:
the cut-off DivX 3 sample has no index and every chunk was a key frame before (now 6 of 1,443).

Suite `tests/suites/51_wmv78.sh`, fixtures in `tests/fixtures/wmv78` (6 files, 196 KB, made by `make.sh` with FFmpeg: odd size,
quantiser 2 and 31, noise for intra macroblocks in P-pictures and inter-intra prediction, WMV8 with the loop filter, in AVI, ASF and
Matroska) plus the six Microsoft MPEG-4 and WMV fixtures of `tests/fixtures/asf`. Judge: FFmpeg 9.0.2.

| Source | What it is | Result against FFmpeg's decode |
|---|---|---|
| 12 committed fixtures | v2, v3, WMV7, WMV8 in AVI, ASF and Matroska | every frame bit-exact, in order |
| encodes made at test time (4 with `QUICK=1`, 15 in all) | v2, v3, WMV7, WMV8 at 176x144 to 720x576, 64 kbit/s to 6 Mbit/s and quantisers 2 and 31, WMV8 with the loop filter, AVI, ASF, Matroska | every frame bit-exact |
| `mpg4.avi` (FATE) | v1, 352x240, 50 frames | bit-exact |
| `div3.avi`, `Park_auto_no_driver.wmv` | v3, 352x240 and 720x576, 50 and 1,142 frames | bit-exact |
| `mp43.wmv` | v3 with slices, 400x250 | 48 frames bit-exact; FFmpeg also decodes the two cut-off media objects at the end (one with an error) |
| Goofy DivX 3 AVI (cut off, no index) | v3, 352x288, 1,443 frames | bit-exact; FFmpeg's demuxer gives one picture unless told to ignore the index (`-fflags +ignidx`), then one cut-off chunk more |
| `asf_msmpeg4v2_mp3_asf2divX_fault.asf` | v2 in a damaged ASF, 1,035 frames | FFmpeg's demuxer stops after 7 pictures, those 7 are identical; the rest look right (checked by eye) |
| `sample-wmv7.wmv`, `wmv1.wmv` | WMV7, 595 and 75 frames | bit-exact |
| `sample-wmv8.1.wmv` | WMV8 with mspel motion, 8x4 and 4x8 transforms, the loop filter, per-macroblock tables, 595 frames | bit-exact |
| FATE `wmv8_x8intra.wmv` | WMV8 with three X8 intra pictures, 474 frames | bit-exact |
| `kelsy_300.wmv`, `sample-wmv9-complex-CP.wmv` | VC-1 Complex profile with X8 intra pictures, 3,314 and 595 frames (refused until now) | bit-exact |

Random frames equal the straight decode and FFmpeg's frame on every file tested (8 to 16 per file), runs of frames after jumps equal
the straight decode, decoded frame times equal ffprobe's, six threads decoding one file each get the straight decode, jumping
around leaks nothing.

Speed, whole program into a pipe, best of three, against `ffmpeg -threads 1` writing raw pictures (FFmpeg has no threads for these
codecs):

| File | Frames | FFamp | FFmpeg | FFamp / FFmpeg |
|---|---|---|---|---|
| v2, 720x576, 3 Mbit/s, noise (made at test time) | 300 | 0.089 s (3,353 fps) | 0.090 s | 1.00 |
| v3, the same | 300 | 0.090 s | 0.091 s | 0.99 |
| WMV7, the same | 300 | 0.090 s | 0.091 s | 1.00 |
| WMV8, the same | 300 | 0.108 s | 0.124 s | 0.87 |
| `Park_auto_no_driver.wmv`, v3 720x576 | 1,142 | 0.179 s (6,380 fps) | 0.165 s | 1.09 |
| `mpg4.avi`, v1 352x240 | 50 | 0.015 s | 0.029 s | 0.53 |
| `sample-wmv7.wmv`, WMV7 CIF | 595 | 0.053 s | 0.062 s | 0.86 |
| `sample-wmv8.1.wmv`, WMV8 CIF | 595 | 0.110 s | 0.124 s | 0.89 |
| `wmv8_x8intra.wmv`, WMV8 with X8 | 474 | 0.051 s | 0.065 s | 0.78 |
| `kelsy_300.wmv`, VC-1 Complex with X8, 320x176 | 3,314 | 0.464 s | 0.433 s | 1.07 |
| `sample-wmv9-complex-CP.wmv`, VC-1 Complex with X8, CIF | 595 | 0.152 s | 0.150 s | 1.01 |

The suite checks the two 720x576 encodes, three samples and `kelsy_300.wmv` against a limit of 1.5. (Without `-fps_mode passthrough`
FFmpeg's raw output repeats pictures to fill the time gaps of some ASF files and takes 0.5 s; the comparison keeps one picture per
packet on both sides.)

The first port took 2.5 times FFmpeg's time on the 720x576 sample; the NEON IDCT, reading code and sign of each coefficient from one
64-bit window (`Vlc::lookup`), run and level packed into the VLC symbol, exact shortcuts for all-zero and DC-only IDCT rows and
`memcpy` for whole-pel copies brought it to FFmpeg's speed.

Fuzzing: the new `msmpeg4` target (codec, size, WMV8 settings and length-prefixed pictures; seeds in
`tests/fixtures/fuzz/msmpeg4`, made from the fixtures by `vc1_check.py seed`) ran 102,795 inputs and the `vc1` target, which now
reaches X8 pictures through the Complex profile flag, 235,868 inputs with undefined-behaviour checks, then 36,678 and 75,419 under
Guard Malloc (heap overruns trap): nothing found. Damaged copies of the fixtures and two samples decode or fail cleanly.

Test counts (after the merge with the WMA 9 Professional and 9 Lossless rounds): `51_wmv78.sh` 64 checks with FFmpeg, 42 without;
with `WINMEDIA_SAMPLES` set it gives 114 (72 without FFmpeg) and `49_vc1.sh` 83 (47 without FFmpeg), all pass; `QUICK=1 tests/run.sh`
2,586 pass, 1 skipped (a real camera); `QUICK=1 NOFFMPEG=1 tests/run.sh` 1,653 pass.

Not done: FFmpeg's error concealment (a picture that stops decoding part way keeps what decoded and takes the rest from the picture
before, FFmpeg guesses motion), so damaged pictures differ from FFmpeg's; WMV8 tools that FFmpeg's encoder cannot make (mspel motion,
8x4 and 4x8 transforms, X8) are covered only by the two sample files, Microsoft MPEG-4 v1 only by the FATE sample, slices (FFmpeg's encoder writes
one per picture) only by `mp43.wmv`; no frame threads (every picture needs the one before; the decoder already matches FFmpeg's one-thread speed).

# Windows Media step G: Windows Media Audio 9 Voice

`src/wmavoice_decode.cpp` decodes Windows Media Audio 9 Voice (`0x000A`) from ASF, AVI and Matroska behind `AudioDecoder`, based on
FFmpeg's decoder (`docs/NOTICE.md`): the CELP helpers it uses and the small transforms of its postfilter are in
`src/wmavoice_dsp.cpp`, its tables in `src/wmavoice_tables.cpp` (converted by script). Rules in `docs/CONVENTIONS.md`.

FFmpeg cannot encode this codec. Fixtures: 5 files in `tests/fixtures/wmavoice` (18 KB) written by `make.py`, FFamp's own bitstream
writer: 8,000, 11,025, 16,000 and 22,050 Hz, 10 and 16 LSPs, independent and residual LSPs, every frame type, with and without the
postfilter, denoise strengths 3, 9 and 11, tilt correction, the DC filter, both quantiser and default-LSP modes, superframes split
across packets, two or three `block_align` units per packet, short superframes (401 and 300 samples), statistics fields, in ASF
(remuxed by FFmpeg), AVI and Matroska; one more has a music superframe. Judge: FFmpeg 9.0.2's `wmavoice` decoder.

Exactness: FFmpeg's own C code built from the reference source (`docs/NOTICE.md` names the files; a scratch harness compiled
`wmavoice.c` and its helpers with this Mac's compiler and ran it with `-cpuflags 0`) gives the same bits as FFamp on all 7 streams
below, postfilter included; the transforms alone were also checked against libavutil's C code on 2,000 random inputs each. The
installed FFmpeg 9.0.2 is newer (libavcodec 63, the reference copy is 62.28) and differs from that build in rare last bits
(first at sample 2,202 of `streaming_CBR-7K.wma` with the postfilter switched off), and its arm64 build runs a NEON FFT in the
postfilter. Against it:

| Stream | FFamp against FFmpeg 9.0.2 (default / `-cpuflags 0`) | FFmpeg 9.0.2's NEON against its own C code |
|---|---|---|
| `streaming_CBR-7K.wma` (8 kHz, 10 LSPs, residual, postfilter, 16.5 s) | 121.9 / 121.9 dB | 136.4 dB |
| FATE `streaming_CBR-11K.wma` (8 kHz, tilt correction) | 121.2 / 121.2 dB | 136.6 dB |
| `streaming_CBR-19K.wma` (16 kHz, 16 LSPs) | 74.7 / 75.6 dB | 82.2 dB |
| 4 fixtures | 93.3 to 112.3 dB | |
| `wmav_8.wma` (music frames inside Voice) | refused with its own message; FFmpeg stops on the same frames (`Not yet implemented`) and decodes the rest | |

Lengths equal FFmpeg's to the sample on every stream. The 19K file is the sensitive one: the postfilter's Kalman search and
denoise table indexes take other branches after a last-bit change, which is why FFmpeg's own two builds differ by 82 dB there.
Suite floors: 90 dB for the fixtures, 70 dB for the sample files.

| Check | Result |
|---|---|
| Random reads (single frames, odd chunk sizes, reads before the start and past the end) | identical to the straight decode on the fixtures, the sample files and a 70 s stream (four 20-second runs, so the marks are used) |
| Probe | `wmavoice`, `flt`, `mono`, other fields as for the other ASF streams |
| Fuzzing with undefined-behaviour checks: `fuzz_wma` on Voice seeds (fixtures and the three sample files) | 2 x 60 s, 76,000 runs, no finds |

Speed, best of three, whole files through the command line (`ffamp adecode` against `ffmpeg -threads 1 ... -f f32le`):

| File | FFamp | FFmpeg, one thread | FFamp / FFmpeg |
|---|---|---|---|
| 600 s written by `make.py` (16 kHz, 16 LSPs, postfilter) | 0.381 s (1,570 times real time) | 0.499 s | 0.76 |
| `streaming_CBR-19K.wma`, 16.5 s | 0.015 s | 0.035 s | 0.41 |
| `streaming_CBR-7K.wma`, 16.5 s | 0.014 s | 0.030 s | 0.46 |

A first version decoded each 20-second run twice on a straight read (the run's mark sat on its first packet, which a straight read
reaches only after the restart that wants it); one preroll packet moved the mark into the run before, which halved the time.

Test counts after the merge with main (WMV7/8): `48_wma.sh` 164 checks with FFmpeg (215 with the sample files of
`WINMEDIA_SAMPLES`), 76 without; `QUICK=1 tests/run.sh` 2,601 pass, 3 skipped (the VC-1 and WMV7/8 sample files, a real camera);
`QUICK=1 NOFFMPEG=1 tests/run.sh` 1,659 pass, 93 skipped.

What is left: music frames (Windows Media Audio 9 Professional inside Voice; FFmpeg has no decoder for them and only one public
sample uses them); last-bit agreement with the installed FFmpeg 9.0.2, whose sources are newer than the reference copy, and with its
NEON FFT.

## Gaps round, milestone 1: sound kinds and containers (`tests/suites/52_sound.sh`)

The audit (`am-private/plans/ffamp52/gap_matrix_2026-11-14.sh`, widened) now decodes every sound kind below in every container
FFmpeg writes it to, each sample bit-exact with FFmpeg 9.0.2 (integer decoders, compared as FFmpeg's `f32le` output):

| Kind | Containers checked |
|---|---|
| A-law, mu-law | WAV, AVI, Matroska, QuickTime, CAF, AIFF-C |
| PCM u8, s8, s16/s24/s32 both byte orders, f32/f64 both byte orders, s64 | every container FFmpeg writes each to (WAV, AVI, Matroska, QuickTime, CAF, AIFF) |
| IMA ADPCM (WAV, 2 to 5 bits, any block size), QuickTime IMA | WAV, AVI, Matroska, QuickTime; QuickTime IMA in QuickTime, CAF, AIFF-C |
| Microsoft ADPCM (1, 2, 6 channels, any block size) | WAV, AVI, Matroska, QuickTime, CAF |
| Yamaha ADPCM | WAV, AVI, Matroska |
| G.722, G.726 (16, 24, 32, 40 kbit/s) | WAV, AVI, Matroska |
| IMA DK3, IMA DK4 (no FFmpeg encoder) | public AVI samples (first 3 MB of each, not committed) |
| FLAC | CAF (no `kuki`), AVI, WAV |
| TTA (u8, s16, 24-bit) | Matroska |

The 2, 3 and 5-bit IMA and 6-channel Microsoft ADPCM fixtures are random payloads in valid blocks (`tests/fixtures/sound/make.py`),
since FFmpeg's encoders write neither. Lossy codecs in the containers newly read go through the system decoders as before; against
FFmpeg: E-AC-3 in AVI 76.9 dB, AC-3 in WAV 76.6 dB, MP2 in WAV 84.2 dB, MP3 in WAV 81.8 dB, AAC in AVI 75.2 dB, Windows Media
Audio 2 in WAV 137.9 dB. Lengths equal FFmpeg's to the sample in every case, including the QuickTime edit list ending inside a
packet and Matroska DiscardPadding on ADPCM.

Speed, 120 s (40 s with QUICK) decoded by `ffamp adecode` against `ffmpeg -threads 1 ... -f f32le`, best of three, ratio of our time to
FFmpeg's: A-law 0.31, IMA ADPCM (WAV) 0.33, QuickTime IMA 0.10, Microsoft ADPCM 0.46, Yamaha 0.67, G.722 0.35, G.726 0.38, TTA 0.71.

Tests after milestone 1 and TTA: `QUICK=1 tests/run.sh` 3,136 pass, 1 skipped (a real camera); `QUICK=1 NOFFMPEG=1 tests/run.sh`
1,854 pass, 94 skipped. `52_sound.sh` alone: 298 checks with FFmpeg, 130 without (with `GAPS_SAMPLES` or `WINMEDIA_SAMPLES` set for
the DK3 and DK4 samples).

# Windows and Linux, round 1 (`docs/PLAN_WINLINUX.md`)

Measured on a Windows 11 PC whose GPU decodes and encodes H.264 and HEVC (HEVC Video Extensions installed), and on Ubuntu 24.04 under
WSL on the same PC. The judge is the FFmpeg 9.0.2 build the app bundles on each system (BtbN GPL builds), so every comparison is with
the FFmpeg the app uses there today.

## Windows

`tests/run.sh` in Git Bash, FFamp built with clang-cl, the libraries with MSVC (no assembly yet):

Full run on the build before the last fixes: 2,392 pass, 109 fail, 184 skip. Reruns of the suites those fixes touched, on the final
build: video 30 pass / 0 fail, audio 163 / 0, writers 369 / 0, tools 127 / 0, capture 207 / 1 (fixed afterwards: the check read
ffprobe's Windows line endings), formats 271 / 73, videox 109 / 6. What still fails: AC-3, E-AC-3, AMR, ProRes, DV and H.263 (no decoder
on Windows yet), the MPEG-1/2 and Motion JPEG checksum tables (made with VideoToolbox; the same files match ffmpeg's decode within the
suite's floors), the Vorbis trim check (FFmpeg's x86 build writes the test file without an end trim, so the check's arithmetic does not
apply), and the speed checks of the Windows Media decoders against FFmpeg on one thread (FFmpeg's x86 build uses SIMD there, FFamp's x86
path is plain C++). Fuzz targets are not built on Windows (the driver is POSIX); those checks are skipped.

Against ffmpeg, frame by frame and sample by sample:

| Path | Result |
|---|---|
| H.264 decode, Media Foundation on the GPU (`mediafoundation-gpu`): six clips, 720p and 1080p, MP4, MOV, fragmented MP4 | every frame bit-exact (4,800 of 4,800), also the H.264 fixtures with B-frames, MBAFF and PAFF |
| HEVC decode, Media Foundation on the GPU: 8 and 10-bit, HDR10 and HLG fixtures | every frame bit-exact |
| 50 random jumps per clip, H.264 and HEVC 720p and 1080p | every frame bit-exact (one wrong HEVC frame after a long backward jump was found and fixed: the decoder now feeds a few more samples instead of draining) |
| MPEG-1 and MPEG-2 decode (Media Foundation, new in this round) | within the suite's floor against ffmpeg in MP4, MOV, Matroska and AVI |
| Motion JPEG (WIC hands out the picture's own YCbCr planes) | within the suite's floor against ffmpeg, 4:2:0, 4:2:2 and 4:4:4 |
| AAC decode (Media Foundation, one frame of delay as lead) | every audio check passes, HE-AAC v1 and v2 included; MP1/MP2/MP3 through mpg123 |
| H.264 and HEVC encode (the GPU driver's encoder MFT) | 1080p at 305 and 333 fps, files clean in ffprobe and FFmpeg's decoder |
| AAC encode (Media Foundation, 96 to 192 kbit/s) | length to the sample, no offset; the encoder starts without a run-in, so FFamp feeds one silent frame and marks it as priming (otherwise the first 1,024 samples decode badly) |
| Software paths with Media Foundation turned off (`FFAMP_NO_MEDIA_FOUNDATION=1`, what Windows N without the Media Feature Pack gets) | OpenH264, libde265, FAAD2, x264 work; HEVC bit-exact, H.264 as on Linux (below) |

## Linux

`tests/run.sh` in WSL, gcc 13, libraries with assembly, final build: 2,345 pass, 175 fail, 188 skip (all fuzz targets and their kept inputs pass).

What is left failing and why:

| Area | Why |
|---|---|
| H.264 with B-frames: decode, jumps, readers, transport streams, frame reader (about 60 checks) | OpenH264 rounds a few samples per B-frame differently next to 8x8 edges (1 to 19 samples off by 1 or 2 per frame, 59 to 101 dB; never a wrong picture; its B-slice deblocking). Plan decision 3. |
| Interlaced H.264 (MBAFF, PAFF fixtures and the 720p MBAFF clip) | OpenH264 refuses it with a message |
| AC-3, E-AC-3, AMR, ProRes, DV, H.263, MPEG-1/2, Motion JPEG (about 90 checks in the formats suite) | system decoders on the Mac; own decoders or libraries are planned for later rounds (PLAN) |
| HE-AAC v2 against ffmpeg | FAAD2's parametric stereo differs (17.6 / 9.5 dB). Plan decision 5. |
| Speed of the WMV7/8 decoder against FFmpeg on one thread | FFmpeg's x86 build uses SIMD for these codecs; FFamp's x86 path is plain C++ |

Fixed along the way: the esds parser was called with the version bytes skipped twice (AAC from FFamp's own writer did not decode),
vo-aacenc's priming is 1600 frames, mpg123 needs `MPG123_NO_READAHEAD` to hand out the first frame at once, OpenH264's drain ends its
stream (the next jump now restarts at a key frame), GIF videos are written without B-frames (FFmpeg's null muxer rejected tied
decode times), and the scaler took the colour matrix of an unlabelled picture from the size after scaling (a 270p clip upscaled to
720p switched from BT.601 to BT.709).

## Linux: MPEG-1/2 through libmpeg2, Motion JPEG through libjpeg-turbo (round 2)

Against ffmpeg's own decode, plane by plane (`tests/comparex.py decode`); both libraries use other IDCTs than FFmpeg, so no frame
is identical, and none is far off:

| Files | Lowest / mean PSNR |
|---|---|
| Fixtures: MPEG-2 in MP4, MOV, Matroska, AVI; MPEG-1 in MOV, Matroska, AVI | 62.8 / 64.7 dB; 63.4 / 65.3 dB |
| Fixtures: Motion JPEG 4:2:0 (MOV, AVI), 4:2:2 (MOV, AVI), 4:4:4 (Matroska) | 67.0 / 67.3; 66.7 / 67.0; 67.3 / 67.9 dB |
| MPEG-1 576p MOV; 203x117 AVI | 62.1 / 63.1; 60.2 / 62.3 dB |
| MPEG-2 576p MP4; open GOP, 3 B-frames, non-linear quantiser, intra VLC (MOV) | 62.1 / 63.1; 62.3 / 63.1 dB |
| MPEG-2 interlaced frame pictures, top first with alternate scan (Matroska), bottom first (AVI) | 62.1 / 63.2 dB |
| MPEG-2 field pictures, top first and bottom first (MP4) | 65.1 / 65.1 dB |
| XDCAM HD422-style 1080i 4:2:2 long GOP (`xd5c` MOV); IMX-style 720x608 4:2:2 intra, 10-bit DC (`mx5p` MOV) | 62.6 / 63.6; 65.0 / 65.1 dB |
| Motion JPEG 4:2:0 AVI, 4:2:2 MOV, 4:4:4 AVI and MOV, 203x117, AVI without Huffman tables | 64.3 to 65.0 dB |

- Random frames equal the straight decode in every file (the fixtures and the 19 made by the suite), so jumps land on ffmpeg's
  pictures within the same margins. A picture size change inside a stream gives each part as its own file does; the sequence
  display extension's colour reaches the frame; a cut that starts inside an open GOP decodes, its first B-pictures shown as the key
  picture they lead to.
- Damaged copies: eight cut or flipped copies of each of the 19 files, and 150 more of eight of them, decode or fail cleanly. A
  sanitised fuzz run of the video target (12 minutes, seeded with MPEG and Motion JPEG files) found
  that libmpeg2 shows a picture from freed buffers when a stream ends right after a sequence header with a new size; nothing is
  shown before the first picture of a sequence now (`tests/fixtures/fuzz/video`).
- Greyscale, 4:1:1, progressive and RGB JPEG pictures were checked by hand (planes 64.6 dB, BGRA 62.9 dB for greyscale, 51.9 dB for
  RGB, which goes through WIC's limited BT.709 conversion).
- Formats suite on Linux: 261 pass, 83 fail before; 298 pass, 46 fail after (the 37 MPEG-1/2 and Motion JPEG checks, the 1 GB
  Motion JPEG AVI among them, now pass; what fails is AC-3, AMR, H.264 through OpenH264 and two alpha checksums). The videox
  suite's new section: 80 pass. The fuzz suite: 23 pass.
- Speed, 250 frames, best of three: MPEG-2 1080i at 25 Mbit/s 0.72 s (ffmpeg 0.25 s with its frame threads, 0.71 s on one thread);
  Motion JPEG 1080p 4:2:2 1.95 s (ffmpeg 2.11 s, 2.73 s on one thread). libmpeg2 decodes on one thread.

## Speed against FFmpeg

Whole-program wall time, best of several runs, nothing else running on the PC. FFamp's tool against FFmpeg's programs doing the same
job the way the app runs them (one program start per thumbnail on both sides; the app itself calls FFamp in its own process, so its
gain is larger than these numbers show). The clips are the suite's 60-second 1080p H.264 (8 Mbit/s) and HEVC (5 Mbit/s) with AAC
sound, and 300 raw 1080p frames for the export.

Windows (Media Foundation on the GPU for H.264 and HEVC, the GPU encoder for H.264 export):

| Task | FFamp | FFmpeg | FFamp faster by |
|---|---|---|---|
| open + probe, H.264 clip | 10 ms | 69 ms | 7.0x |
| open + probe, HEVC clip | 11 ms | 29 ms | 2.7x |
| frame index (packet list), H.264 / HEVC | 91 / 68 ms | 129 / 76 ms | 1.4x / 1.1x |
| 12 thumbnails, H.264 / HEVC | 3.0 / 6.3 s | 1.8 / 3.4 s | 0.60x / 0.54x |
| decode 1,800 frames to memory, H.264 | 4.1 s | 1.4 s (software), 2.3 s (`-hwaccel d3d11va`) | 0.34x / 0.56x |
| decode 1,800 frames to memory, HEVC | 5.8 s | 2.6 s (software), 2.4 s (d3d11va) | 0.44x / 0.42x |
| trim 20 s from 10 s, H.264 / HEVC | 34 / 32 ms | 99 / 52 ms | 2.9x / 1.6x |
| extract audio by copy, H.264 / HEVC clip | 22 / 23 ms | 92 / 53 ms | 4.1x / 2.3x |
| extract audio to MP3 128 kbit/s | 338 ms | 295 ms | 0.87x |
| export MP4, 300 frames 1080p H.264 8 Mbit/s + AAC 128 kbit/s | 981 ms | 794 ms (FFmpeg's vendor encoder for this GPU, `aac_mf`) | 0.81x |

Linux (software libraries; FFmpeg's own decoders use frame threads):

| Task | FFamp | FFmpeg | FFamp faster by |
|---|---|---|---|
| open + probe, H.264 / HEVC clip | 1 / 2 ms | 55 / 13 ms | 37x / 8.3x |
| frame index, H.264 / HEVC | 64 / 44 ms | 63 / 19 ms | 0.98x / 0.44x |
| 12 thumbnails, H.264 / HEVC | 4.1 / 9.4 s | 2.2 / 4.0 s | 0.54x / 0.42x |
| decode 1,800 frames to memory, H.264 (OpenH264) | 17.3 s | 1.5 s | 0.09x |
| decode 1,800 frames to memory, HEVC (libde265) | 11.2 s | 2.8 s | 0.25x |
| trim 20 s from 10 s, H.264 / HEVC | 21 / 21 ms | 83 / 38 ms | 4.0x / 1.8x |
| extract audio by copy, H.264 / HEVC clip | 8 / 7 ms | 83 / 36 ms | 10x / 5.0x |
| extract audio to MP3 128 kbit/s | 354 ms | 375 ms | 1.06x |
| export MP4, 300 frames 1080p H.264 8 Mbit/s + AAC 128 kbit/s | 1.43 s | 1.43 s (`libx264` veryfast, `aac`) | 1.00x |

What the numbers say:

- Opening, probing, trimming and copying sound are faster on both systems, as on the Mac and Android: FFamp's readers start at once.
- Decoding is where FFamp loses on these two systems today. On Linux OpenH264 decodes on one thread (104 fps for this noisy 1080p
  clip against FFmpeg's 1,200 fps with frame threads); its own frame threads lost frames at the end of the stream in a test, so they
  are off. libde265 uses its slice threads only. On Windows the GPU decodes, but every picture is copied back through Media Foundation,
  copied into FFamp's frame and turned into I420 on one thread (about 435 fps for H.264, 310 for HEVC). Thumbnails follow the decoder,
  plus a Media Foundation start per program here. In the app FFamp runs in its own process, so the per-program start disappears, but
  the decode speed remains the gap to close: OpenH264 frame threads, a second decoder instance ahead in the next group, or VA-API on
  Linux; on Windows handing the D3D11 texture to the app without the copies.
- Exporting is level on Linux (x264 on both sides) and 19 % slower on Windows, where FFmpeg drives the GPU through the vendor's own
  interface and FFamp through the vendor's Media Foundation MFT.

Commands: `tests/speed.py <folder with the suite's clips>`, best of two to five runs; FFmpeg is the BtbN 9.0.2 build the app bundles.

# Windows and Linux, round 2: audio (`docs/PLAN_WINLINUX.md`)

Same PC and judge as round 1: Windows 11 and Ubuntu 24.04 under WSL, FFmpeg 9.0.2 (the BtbN build the app bundles) on each system.

What this round added on both systems:

- **AAC: FFamp's own decoder**, based on FFmpeg's float decoder (`src/aac_decode.cpp`, `src/aac_sbr.cpp`, `src/aac_tables.cpp`,
  `src/aac.h`): AAC-LC, HE-AAC (SBR) and HE-AAC v2 (parametric stereo), every channel layout up to eight channels (the default
  configurations and PCEs), explicit and implicit SBR and PS signalling (HE-AAC mono comes out as stereo, as in FFmpeg), in MP4, M4A,
  MOV, ADTS, Matroska, MPEG-TS and CAF. It runs on Linux always and on Windows where Media Foundation is missing or refuses the
  stream; Media Foundation stays first on Windows. FAAD2 is gone from the build, the notices and the plan.
- **AC-3 and E-AC-3: FFamp's own decoder**, based on FFmpeg's (`src/ac3_decode.cpp`, `src/ac3_tables.cpp`), on both systems and every
  Windows edition.
- **AMR-NB and AMR-WB through opencore-amr** 0.1.6 (Apache-2.0, decoders only, static from the pinned tarball;
  `src/opt/amr_decode.cpp`, `third_party/NOTICE_opencore-amr.md`).

## Against FFmpeg

| Codec | What was checked | Against ffmpeg's default decoder |
|---|---|---|
| AAC-LC | 16 layouts (mono to 7.1 and 7.1(wide), PCE layouts included), 8 to 96 kHz, 24 to 384 kbit/s, PNS, intensity stereo, M/S, TNS, both coders of FFmpeg's encoder; MP4, M4A, MOV, Matroska, TS, ADTS, CAF; the Apple-made fixture | 138.5 to 139.9 dB, length and timing to the sample |
| HE-AAC | Apple-made files: mono, stereo at 22.05, 32, 44.1 and 48 kHz, 5.1; in M4A, ADTS, CAF and remuxed into Matroska, TS and MOV | 132.1 to 135.0 dB; 104.1 dB at 22.05 kHz |
| HE-AAC v2 | Apple-made files at 32, 44.1 and 48 kHz in M4A and ADTS, remuxed into Matroska, TS and MOV | 134.3 to 135.4 dB |
| AC-3, E-AC-3 | every fixture, every acmode with and without LFE at 32, 44.1 and 48 kHz, low and high rates, raw, MP4, MOV, Matroska, TS, AVI and WAV, and the syntax FFmpeg's encoders never write (suite `53_ac3`) | 138.2 dB or more on every file of the suite, length and timing to the sample |
| AMR-NB | the fixtures in AMR, 3GP and CAF, every mode from 4.75 to 12.2 kbit/s with and without DTX | bit-exact against `libopencore_amrnb`; 32.7 dB against FFmpeg's own float `amrnb` |
| AMR-WB | the fixtures in AMR and 3GP (FFmpeg has no AMR-WB encoder to make more) | bit-exact against `libopencore_amrwb`; 34.7 dB against FFmpeg's own float `amrwb` |

HE-AAC is as close to FFmpeg as FFmpeg is to itself. The same FFmpeg build run with `-cpuflags 0` (its C code instead of its
SSE/AVX2/FMA3 code) differs from its normal output by the same amount on every file:

| File | FFmpeg C against FFmpeg SIMD | FFamp against FFmpeg SIMD |
|---|---|---|
| HE-AAC stereo 32 kHz / 44.1 kHz (ADTS) / 48 kHz | 135.1 / 135.1 / 133.5 dB | 135.0 / 135.0 / 133.7 dB |
| HE-AAC mono (out as stereo) 44.1 kHz | 134.8 dB | 134.8 dB |
| HE-AAC 5.1 48 kHz | 132.3 dB | 132.1 dB |
| HE-AAC stereo 22.05 kHz | 104.3 dB | 104.1 dB |
| HE-AAC v2 32 / 44.1 (ADTS) / 48 kHz | 134.6 / 134.3 / 135.7 dB | 134.3 / 134.3 / 135.4 dB |
| AAC-LC stereo 44.1 kHz (ADTS) | 140.3 dB | 138.9 dB |

FAAD2, which this decoder replaces, was 17.6 and 9.5 dB from FFmpeg on HE-AAC v2. The decoder follows FFmpeg's float code and, where
FFmpeg's x86 code orders or fuses the arithmetic differently from its C code (the SBR sums, autocorrelation and high-frequency
generator, the PS hybrid filters, the QMF synthesis window with FMA3, the PNS energy), FFamp computes in that order, with FMA where the
CPU has FMA3 as FFmpeg does. What remains is the MDCT: FFmpeg's is AVX2 assembly with FMA3, FFamp's runs in double precision. The
22.05 kHz file sits at 104 dB for both comparisons because SBR's inverse filtering is ill-conditioned there (FFmpeg's own comment
calls it numerically unstable). The suite therefore asks 135 dB of AAC-LC and, for HE-AAC, no more than 1 dB below what FFmpeg's C
code reaches against FFmpeg (`audio.py against ... peer`).

Random reads over long HE-AAC and HE-AAC v2 files equal one straight read to the bit across the 20-second restart points: the decoder
saves its SBR and PS state at each restart point (about 260 KB for each channel element that uses SBR).

On Windows the suite runs FFamp's AAC decoder with `FFAMP_NO_SYSTEM_AUDIO=1`; with Media Foundation in front, every AAC check of
round 1 still passes.

## Decoding speed against FFmpeg

120 seconds of sound, whole-program wall time, best of five, one thread on both sides (`ffmpeg -threads 1 ... -f f32le -` against
`ffamp adecode`). The Linux FFmpeg takes about 0.18 s to start, which flattens its short runs.

| Codec | Windows: FFamp / FFmpeg | Linux: FFamp / FFmpeg |
|---|---|---|
| AAC-LC 5.1 384 kbit/s | 0.224 / 0.141 s (0.63x) | 0.213 / 0.255 s (1.19x) |
| AAC-LC stereo 128 kbit/s | 0.086 / 0.093 s (1.08x) | 0.071 / 0.196 s (2.75x) |
| HE-AAC stereo 48 kbit/s | 0.221 / 0.138 s (0.62x) | 0.189 / 0.185 s (0.98x) |
| HE-AAC v2 32 kbit/s | 0.213 / 0.150 s (0.71x) | 0.184 / 0.205 s (1.11x) |
| AC-3 5.1 448 kbit/s | 0.154 / 0.143 s (0.93x) | 0.150 / 0.231 s (1.54x) |
| E-AC-3 5.1 448 kbit/s | 0.157 / 0.142 s (0.91x) | 0.150 / 0.230 s (1.53x) |
| AMR-NB 12.2 kbit/s | 0.092 / 0.108 s (1.17x) | 0.077 / 0.234 s (3.06x) |
| AMR-WB 23.85 kbit/s | 0.204 / 0.132 s (0.65x; FFmpeg's `libopencore_amrwb` 0.208 s) | 0.173 / 0.246 s (1.42x) |

The factor is FFmpeg's time over FFamp's (above 1: FFamp is faster). On Windows the AAC decoder spends most of its time in the MDCT,
which FFmpeg runs in AVX2 assembly. Media Foundation, which decodes AAC there first, takes 0.267 s for the 5.1 file, 0.106 s for the
stereo one, 0.176 s for HE-AAC and 0.191 s for HE-AAC v2. AMR-WB is opencore's reference decoder: as fast as FFmpeg's
`libopencore_amrwb`, slower than FFmpeg's own float decoder.

Command: `tests/speed_audio.py <work folder>` from `tests/` with `FFMPEG` and `AMEDIA` set; the AAC suite also checks long files
against ffmpeg's time (2x for 5.1 AAC-LC, 1.5x for HE-AAC).

## Suites

Full runs at the end of the round (`tests/run.sh`):

- **Linux (WSL): 2,729 pass, 131 fail, 200 skip** (round 1: 2,345 / 175 / 188). The audio suites pass whole: `20_audio`
  163 / 0, `52_sound`, `53_ac3`, `54_aac` 85 / 0, and every AAC, AC-3 and AMR check of `45_formats`. What fails is video (OpenH264's
  B-frame rounding and interlace, MPEG-1/2, Motion JPEG, DV, ProRes, H.263, which the video rounds handle) and `rd_vorbis.ogg trims to
  the length ffmpeg decodes`, which failed in round 1 as well.
- **Windows: 2,789 pass, 42 fail, 206 skip** (round 1: 2,392 / 109 / 184). Again every audio check except two that failed in round 1
  too: the WMA 2 speed limit (1.97x and 2.26x FFmpeg's time against a limit of 1.5x; round 1 measured 1.81x and 2.18x) and
  `rd_vorbis.ogg`. The rest is video: Motion JPEG, MPEG-1/2, DV, H.263, ProRes, VP8/VP9 alpha, `bad_sample.mp4` and one remux check.

## Left after this round

- AAC Main and LTP profiles, the ER, LD, ELD and USAC object types, 960-sample frames and coupling channel elements are refused with a
  message; FFmpeg decodes them. None was met in the app's files; Media Foundation still covers AAC Main on Windows.
- AAC files that carry libfaac's version string have their first 1,024 samples dropped by FFmpeg and not by FFamp.
- AAC-LC takes 1.6 times FFmpeg's time on Windows (the MDCT, see above); a faster double-precision FFT would close most of it.
- The SBR and PS state saved at restart points costs about 260 KB per SBR channel element for every 20 seconds read.

# Windows and Linux, round 2: video

Same PC and judge as round 1 (FFmpeg 9.0.2, BtbN GPL builds). New in this round: NASM builds of the Windows libraries, faster
Windows decoding, VA-API on Linux, MPEG-1/2 and Motion JPEG on Linux, and own ProRes, DV and H.263 decoders on both systems.

## Accuracy of the new decoders

| Decoder | Systems | Against ffmpeg |
|---|---|---|
| ProRes (own, from FFmpeg's) | Windows, Linux | bit-exact: every profile from proxy to 4444 XQ, alpha at 16 and 8 bits, interlaced both field orders, odd sizes, two encoders; random frames and damaged copies (videox suite) |
| DV (own, from FFmpeg's) | Windows, Linux | bit-exact: PAL, NTSC, 2-4-8 fields, DV50, DVCPRO HD 1080i60/1080i50/720p60/720p50, in MOV and AVI |
| H.263 and H.263+ (own, on the H.263 core) | Windows, Linux | bit-exact on 20 encodes (3GP, MOV, AVI, Matroska; custom sizes, GOBs, slices, 4MV, OBMC, AIC, deblocking, UMV, alternative inter VLC, modified quantisation), seeks and damaged copies (`53_h263`, 49 checks) |
| MPEG-1/2 (libmpeg2) | Linux | 60 to 65 dB (libmpeg2's SIMD IDCT rounds differently; the libmpeg2 section above) |
| Motion JPEG and JPEG (libjpeg-turbo) | Linux | 64 to 68 dB (the same section) |
| VA-API H.264, HEVC, VP9, AV1 | Linux | bit-exact (below) |
| Media Foundation H.264 and HEVC with FFamp's own copy-back | Windows | bit-exact, unchanged (every frame of the 720p and 1080p clips, 50 random jumps per clip) |

## VA-API on Linux: what was tested, and what could not be

Tested in WSL (Ubuntu 24.04) with Mesa 25.2.8's d3d12 VA-API driver on the PC's integrated GPU, against ffmpeg frame by frame:

- `61_vaapi` (new suite, encoded by the judge at test time): H.264 with B-frames, weighted prediction, MBAFF, four slices and an
  odd size; HEVC Main and Main 10; VP9 profiles 0 and 2; AV1 8 and 10-bit. Every frame bit-exact, 12 random frames per file equal
  to the straight decode, `FFAMP_NO_VAAPI` falls back, and the three d3d12 fallbacks below stay on software and decode right.
- The VA-API helpers' clip sets, rerun on the merged branch: 83 files (15 H.264 from x264
  with every frame type and reference case, about 40 HEVC from x265 and kvazaar plus crafted long-term, RPS, scaling-list, tile and
  weighted streams, 24 AV1 from aom, SVT-AV1 and rav1e with tiles, superres, intra block copy, loop restoration, film grain and 10-bit).
  80 are bit-exact (77 on the GPU, 3 sent to dav1d by the fallbacks). The other three: the H.264 file with scaling matrices goes to
  OpenH264 (its B-frame rounding, 71.9 dB); x265 with 16x16 CTUs is 85 dB from ffmpeg, and libde265 gives exactly the same pictures
  as the GPU, so FFmpeg is the odd one there; a crafted HEVC file that ffmpeg itself reports broken ("No ref lists in the SPS")
  differs in one frame. (22 raw `.hevc` streams of the set were not run: FFamp has no raw HEVC reader; their MP4 copies are above.)
- The fixtures and 1080p clips of the video, readers, formats and frame-reader suites now decode H.264 and HEVC on the GPU: the
  H.264 B-frame and interlaced failures of round 1 (OpenH264) are gone where VA-API runs.

Found and handled (the d3d12 driver, not FFamp): it ignores H.264 scaling matrices (24.7 dB in either list order), it decodes
10-bit VP9 and AV1 wrongly when the height % 16 is 2 to 9 (1080 lines included; found by the new suite for VP9), and AV1 film grain
is left to dav1d; those streams stay on the software decoders.

Not tested: a native Linux install with Intel's iHD or AMD's radeonsi VA-API drivers (no such machine here; the code path is the
same, only the d3d12 workarounds are skipped there), NVIDIA (no VA-API in its driver; NVDEC is described in the plan), H.264 High 10 and
4:2:2 (the VA-API path takes 8-bit 4:2:0 only; the rest goes to software), HEVC range extensions,
VP9 profiles 1 and 3, AV1 High/Professional (software), and VA-API encoding (not part of this round).

## Windows decode speed

What cost the time, measured in the program (`ffamp bench`, 1080p H.264 clip): Media Foundation's own copy-back (`Lock2D` on the
GPU buffer) waited for each picture, 466 fps with copies against 1,160 fps without; then FFamp copied the two-plane picture and split
it on one thread. And at the first stream of every process, Microsoft's H.264 decoder sat 0 to 1.4 s in `SetThreadAffinityMask`
(stack samples during `MFT_MESSAGE_NOTIFY_BEGIN_STREAMING`; a second decoder in the same process started in 1 ms), which made
program starts slow and uneven.

Now: FFamp copies each GPU picture into a ring of three staging textures and maps it two pictures later, skips the copy for
pictures it will not keep (the run-up to a jump target), splits the planes on the pixel crew while copying in, shares one D3D11
device per process, creates Microsoft's decoders by class ID and keeps their DLL away from `SetThreadAffinityMask`. In the program
the copies now cost 2 % (1,140 fps with copies, 1,160 without), which is the GPU's own limit for this clip: two decodes at once take
twice as long, in FFamp and in FFmpeg alike.

Whole programs on the idle PC, best of two or three runs, `tests/speed.py` (the other rows of the round 1 table moved by a few
milliseconds only). "Before" is round 1's table; the merged branch measured again the same day before this work gave 4.5 s and 5.9 s
for decoding and 10.9 s and 6.4 s for the thumbnails (the affinity stall was worse that day).

| Task (1080p, 60 s, 1,800 frames) | FFamp before | FFamp now | FFmpeg software | FFmpeg `-hwaccel d3d11va` | Now against FFmpeg |
|---|---|---|---|---|---|
| decode to memory, H.264 | 4.1 s | 1.72 s | 1.45 s | 2.60 s | 0.85x software, 1.51x GPU |
| decode to memory, HEVC | 5.8 s | 2.01 s | 2.68 s | 2.63 s | 1.34x software, 1.31x GPU |
| 12 thumbnails, H.264 (a program start each) | 3.0 s | 2.22 s | 1.88 s | | 0.85x |
| 12 thumbnails, HEVC | 6.3 s | 3.70 s | 3.58 s | | 0.97x |
| open + probe, H.264 / HEVC | 10 / 11 ms | 13 / 14 ms | 71 / 33 ms | | 5.3x / 2.4x |
| export MP4, 300 frames H.264 + AAC | 981 ms | 1,050 ms | 834 ms (the GPU vendor's encoder) | | 0.79x |

H.264 decode to memory stays behind FFmpeg's software decoder (1.45 s): the GPU itself decodes this clip at about 1,150 frames per
second, two decoders at once share that rate, and Microsoft's software decoder reaches 700 fps (152 in the low-latency mode FFamp
uses). Beating FFmpeg there needs a fast bit-exact software H.264 decoder next to the GPU (the owner's own port, separate branch).
HEVC and every GPU comparison are ahead. Seeking in the program (`10_video`, 50 random frames): H.264 1080p 66 to 30 ms, HEVC 1080p
406 to 141 ms (FFmpeg's program 140 and 287 ms per jump).

In the app FFamp runs inside its own process, so a thumbnail costs only the jump (30 ms for H.264 1080p, 141 ms for HEVC above, the
device and decoder start once per process); the program-start rows above are what the CLI pays each time.

## NASM on Windows: the libraries with and without assembly

NASM 3.02, the portable `nasm-3.02-win64.zip` from nasm.us (641,314 bytes, SHA-256
`161d0bfaff53c2f9e9f3e69fd0672323ebabafd1268976a5cec11be92a19aee7`; nasm.us publishes no checksum file), unpacked next to the build,
never shipped. `tests/speed.py --libraries` with Media Foundation turned off, the same FFamp source linked once against the libraries
built without assembly and once with:

| Library task (1080p) | Without assembly | With NASM | Gain | FFmpeg |
|---|---|---|---|---|
| VP9 decode to memory, 60 s (libvpx) | 12.7 s | 2.60 s | 4.9x | 2.23 s |
| AV1 decode to memory, 60 s (dav1d) | 4.51 s | 1.86 s | 2.4x | 2.10 s |
| H.264 decode, 60 s (OpenH264, the fallback without Media Foundation) | 29.7 s | 26.2 s | 1.13x | 1.44 s |
| HEVC decode, 60 s (libde265, the fallback) | 12.9 s | 13.0 s | none (libde265 has no NASM code) | 2.67 s |
| H.264 encode, 300 frames (x264) | 1.64 s | 0.73 s | 2.2x | 0.75 s |
| HEVC encode, 300 frames (x265) | 23.3 s | 4.05 s | 5.8x | 3.68 s |
| AV1 encode, 300 frames (SVT-AV1) | 6.55 s | 1.10 s | 6.0x | 1.09 s |
| VP9 encode, 300 frames (libvpx) | 3.29 s | 1.38 s | 2.4x | 1.38 s |

With assembly the encoders are level with FFmpeg's, dav1d is ahead, libvpx 14 % behind (FFmpeg uses its own VP9 decoder with
frame threads). The software H.264 and HEVC fallbacks stay slow on one thread; Media Foundation covers both on Windows.

## Linux speed with VA-API (WSL)

`tests/speed.py` in WSL, VA-API on (now) and with `FFAMP_NO_VAAPI=1` (the software libraries, as in round 1):

| Task (1080p, 60 s) | Round 1 (software) | VA-API now | Software now | FFmpeg |
|---|---|---|---|---|
| decode to memory, H.264 | 17.3 s | 8.84 s | 18.0 s (OpenH264) | 1.54 s |
| decode to memory, HEVC | 11.2 s | 9.93 s | 10.8 s (libde265) | 2.93 s |
| 12 thumbnails, H.264 | 4.1 s | 3.93 s | 4.35 s | 2.17 s |
| 12 thumbnails, HEVC | 9.4 s | 5.21 s | 9.45 s | 4.03 s |

The GPU decodes these clips at 1,110 (H.264) and 985 (HEVC) frames per second inside WSL when nothing is copied out, as fast as Media
Foundation on the same GPU, but each picture copied out through the d3d12 driver (`vaGetImage`, through WSL's GPU
paravirtualisation) costs about 4.5 ms: 199 and 181 fps with copies. `vaDeriveImage` was tried (234 and 207 fps) and not kept, since
on native drivers it maps tiled video memory directly. A native Linux machine with Intel or AMD drivers was not available to
measure. Skipping the copies for pictures that are not kept made jumps on the GPU 3 to 4 times faster here (`10_video`, 50 random
frames: H.264 1080p 148 to 43 ms, HEVC 1080p 655 to 147 ms).

## Suites at the end of round 2

| System | Pass | Fail | Skip |
|---|---|---|---|
| Windows (Git Bash, clang-cl, libraries with NASM) | 2,644 | 51 | 209 |
| Linux (WSL, gcc 13, VA-API through d3d12) | 2,804 | 53 | 189 |

Round 1 had 2,392 / 109 / 184 on Windows and 2,345 / 175 / 188 on Linux (that Linux run stopped at the stand-in suite, so the fixes,
speed check and speed suites were not counted). The two full runs ran at the same time on the one PC; the formats suite was run again
alone after its desktop checksums were added (the VP8 and VP9 alpha BGRA tables and the damaged-sample file, which Windows and Linux
now decode identically; the Mac tables are VideoToolbox's). What still fails:

| Checks | Why |
|---|---|
| AC-3, E-AC-3, AMR, HE-AAC in CAF and the AAC channel-order fixes (45 on Windows, 46 on Linux) | audio decoders of the audio branch, not in this one |
| HE-AAC v2 against ffmpeg (Linux) | FAAD2's parametric stereo (round 1, plan decision 5) |
| Vorbis trim (both) | FFmpeg's x86 build writes the test file without an end trim (round 1) |
| WMV7/8 and WMA 2 speed against FFmpeg on one thread (1 on Linux, 3 on Windows) | FFmpeg's x86 SIMD for these codecs (round 1) |
| Speed check, 10-bit HEVC 150 frames over 800 fps as a whole program (Windows: 417 fps) | a Mac threshold: the program start alone (D3D11 device, about 0.2 s) exceeds it |
| Speed check: RGBA frames, 10-bit HEVC and AV1 software rates (4 on Linux) | WSL's slow VA-API copy-out above (120 to 124 fps for RGBA frames), dav1d 366 fps against 400 |
| `h264_aac.ts` decodes like `h264.mp4` (Windows, once) | failed once in the full run while WSL was using the GPU; 90 reruns, also with the GPU busy, all exact |

# Windows and Linux, round 3: FFamp's own H.264 decoder (`src/h264`)

FFmpeg 9.0.2's H.264 decoder ported to FFamp in plain C, kept file for file close to FFmpeg's (`h264dec.c`, `h264_slice.c`,
`h264_cabac.c` and the rest, comments taken out) over a small libavutil layer (`ffbase.h`, `ffbase.c`: memory, reference counting,
frames, pixel formats). FFmpeg's frame threads and slice threads are rewritten in `h264_ffamp.c` with the same rules
(`update_thread_context`, setup and row progress, round-robin output); FFmpeg's x86-64 assembly (NASM, SSE2 to AVX2, 8 and 10-bit
IDCT, deblocking, intra prediction, quarter-pel, chroma MC, weights, edge emulation) and AArch64 NEON are built in, with FFmpeg's inline
CABAC on both. Error concealment is FFmpeg's (`error_resilience.c`). H.274 film grain is synthesised as FFmpeg does by
default (`h274.c`). It replaces OpenH264 on Linux and behind Media Foundation on Windows. Not ported: the export of side data
(captions, HDR metadata, stereo and display matrices; the T.35 user data SEI is skipped), hardware acceleration.

## Bit-exactness

Judge: the BtbN FFmpeg 9.0.2 the app bundles (Windows, WSL), Homebrew FFmpeg 9.0.2 on the Mac for the NEON build; frame against frame
with `-fps_mode passthrough` in the decoder's own pixel format.

| Material | Result |
|---|---|
| 36 x264 encodes made by suite 54_h264: Baseline, Main, High, High 10, High 4:2:2 (8 and 10-bit), High 4:4:4 (8 and 10-bit, CAVLC and CABAC), lossless 4:4:4 and 4:2:0, RGB (`libx264rgb`, G, B and R planes), monochrome, MBAFF (top and bottom field first, CABAC and CAVLC, 4:2:0, 4:2:2, 10-bit), B-pyramids, temporal and spatial direct, weighted prediction, 16 references, open GOP, intra refresh, constrained intra, slices by count and by size, odd sizes, left and right cropping, 720p | every frame bit-exact with one thread, frame threads and slice threads, on Windows (clang-cl, assembly), Linux (gcc, assembly) and the Mac (arm64 NEON); also with the assembly turned off (`FFAMP_H264_CPU=0`) |
| H.274 film grain SEI written into an x264 stream, on every picture and once with a repetition period | every frame bit-exact (FFmpeg applies the grain; with `-export_side_data film_grain` its pictures differ, so the grain is really there) |
| Fixtures (H.264 in MP4 and AVI, MBAFF, PAFF field pairs) | every frame bit-exact; random frames through the VideoDecoder equal ffmpeg's (50 per clip, 5 clips) |
| Damaged copies (bits flipped and runs zeroed inside `mdat`), 36 per run in three layouts | all decode with one thread and with frame threads; with one thread bit-exact against ffmpeg on one thread except where FFmpeg's own output depends on unwritten frame memory (a macroblock whose reference is invalid keeps whatever the buffer held): there FFmpeg's result changes with its own command line (adding `-vf null` changes it), and FFamp's with `FFAMP_H264_FILL`. With frame threads FFmpeg is not deterministic on damaged streams (four runs, four results), FFamp neither |
| Public conformance bitstreams (JVT suite) | not run: fetching them needs the owner's go; `H264_CONFORMANCE=<folder> tests/run.sh` runs them |

The damaged-stream comparison also shows why the assembly matters for parity: FFmpeg's SIMD IDCT treats out-of-range coefficients
differently from its C code, and its arm64 CABAC checks the end of the slice where the C and x86 code do not; with the assembly ported,
FFamp follows the FFmpeg of the same system.

Fuzz target `fuzz_h264` (packets with two-byte lengths, 1 to 4 frame threads or slice threads, flushes in between, five kept
inputs): 300,000 runs on the Mac and 62,500 under AddressSanitizer in WSL without a crash, a hang or a memory error. 159 decodes of the
test clips, the damaged copies and the fixtures (one thread, frame threads, slice threads) under AddressSanitizer with leak checks and
UndefinedBehaviorSanitizer (FFmpeg's deliberate signed shifts and wrap-around excepted): no error.

## Suites

| System | Full run | Round 1 | H.264 |
|---|---|---|---|
| Linux (WSL, gcc 13, assembly) | 2,469 pass, 109 fail, 189 skip | 2,345 / 175 / 188 | no H.264 failure left (the ~60 OpenH264 B-frame checks and the interlaced fixtures now pass); 54_h264 all pass |
| Windows (clang-cl, NASM 3.02 assembly) | 2,463 pass, 85 fail, 196 skip | 2,392 / 109 / 184 | none; 54_h264 all pass (57, the fuzz target is not built on Windows) |

What still fails on both is outside H.264: AC-3, E-AC-3, AMR, ProRes, DV, H.263, MPEG-1/2 and Motion JPEG (decoders of the other
round 2 helpers, not on this branch), HE-AAC v2 on Linux, the Vorbis trim check, the alpha VP8/VP9 BGRA checksums, `bad_sample.mp4`'s
checksum (made from VideoToolbox's concealment of its broken picture), the 1 GB Motion JPEG AVI on Linux and the one-thread speed checks
of the Windows Media decoders.

## Speed against FFmpeg

The suite's 60 s 1080p clip (8 Mbit/s, noisy, x264 veryfast with B-frames), best of three, nothing else running on the PC; FFmpeg is the
BtbN 9.0.2 build the app bundles. "Decoder only" is `ffamp h264 --null` against `ffmpeg -f null`; "decode to memory" is the
`tests/speed.py` job (`ffamp decode` against `ffmpeg -f rawvideo`).

| Job | Linux (WSL) FFamp | FFmpeg | Windows FFamp | FFmpeg |
|---|---|---|---|---|
| decoder only, one thread | 6.93 s | 7.07 s | 9.71 s | 10.34 s |
| decoder only, threads (CPU count + 1) | 1.24 s | 1.38 s | 1.12 s | 1.29 s |
| decode to memory, FFamp's H.264 decoder | 1.40 s (1.13x) | 1.59 s | 1.32 s (1.20x, `--software`) | 1.58 s |
| 12 thumbnails (one program start each) | 0.86 s (2.53x) | 2.16 s | 3.23 s with Media Foundation | 1.84 s |

Round 1 had 17.3 s (0.09x) for the Linux decode and 4.1 s for Linux thumbnails. On Windows the default path is still Media Foundation
on the GPU (4.13 s for the decode, 0.36x, because every picture comes back from the GPU through Media Foundation); FFamp's decoder on
the CPU decodes the same clip in 1.32 s, faster than FFmpeg's software decoder (1.58 s) and its D3D11VA path (2.66 s).

arm64 (the Mac's M4, NEON build, a 10 s 1080p clip, decoder only): 0.17 to 0.20 s with threads against `ffmpeg -f null`'s 0.33 s (a
program start included), 0.84 s against 0.89 s on one thread; without NEON 1.08 s. Linux arm64 itself was not measured (no such
machine here).

The pictures go to the `VideoDecoder` without a copy: `Picture.keep` holds the decoder's frame and the `Image` points at its planes.
Copying each 1080p picture cost about a fifth of the decode time.

# Windows and Linux, round 2 merged: audio, video and the own H.264 decoder together

The three round 2 branches (`wl-audio`, `wl-h264`, `wl-video`) merged into `winlinux`. Every H.264 software fallback goes to FFamp's
own decoder: VA-API refusals on Linux (more than 8 bits, not 4:2:0, the d3d12 driver's scaling-matrix guard) and Media Foundation
refusals on Windows. OpenH264 and FAAD2 are out of the build scripts, the notices and the plan. Media Foundation stays the first H.264
decoder on Windows and VA-API the first on Linux. Same PC and judge as before (FFmpeg 9.0.2, BtbN builds); the libraries were built
again from the merged build scripts (Windows with NASM).

## Suites

Full runs of `tests/run.sh` (pass / fail / skip):

| System | Merged | `wl-audio` | `wl-video` | `wl-h264` | Round 1 |
|---|---|---|---|---|---|
| Windows (clang-cl, libraries with NASM) | 3,087 / 6 / 223 | 2,789 / 42 / 206 | 2,644 / 51 / 209 | 2,463 / 85 / 196 | 2,392 / 109 / 184 |
| Linux (WSL, gcc 13, VA-API through d3d12) | 3,248 / 8 / 202 | 2,729 / 131 / 200 | 2,804 / 53 / 189 | 2,469 / 109 / 189 | 2,345 / 175 / 188 |

The audio and H.264 branch runs stopped at the stand-in suite on both systems (as round 1's Linux run did; `wl-video` fixed that), so
the fixes, speed check and speed suites only ran on `wl-video` and here. Nothing the merge touched fails:

| Check | System | Why |
|---|---|---|
| `rd_vorbis.ogg` trims to the length ffmpeg decodes | both | FFmpeg's x86 build writes the test file without an end trim (round 1) |
| WMA 2 600 s speed (2 checks), `wv_speed_v3.avi` speed | Windows; `wv_speed_v3.avi` on both | FFmpeg's x86 SIMD for these codecs (round 1) |
| `fixes: channel-order` | both | FFamp's own ALAC and AAC decoders give 6.1 in FFmpeg's order (FL FR FC LFE SL SR BC), the convention of the Mac decoders and this check is FL FR FC LFE BC SL SR. `54_aac`'s 6.1 check asks for FFmpeg's order, so the two checks disagree; failed on `wl-video` too |
| `fixes: FFmpeg decodes 100 frames from the 100-frame AAC file` | Linux | vo-aacenc's 1,600 frames of priming put the whole 100-frame edit inside the shortened last packet; FFmpeg's MP4 reader marks both packets discarded and decodes nothing. Media Foundation (1,024 frames of priming) and AudioToolbox pass; failed on `wl-video` too |
| Speed check: 1080p RGBA frames (2 checks), 10-bit HEVC over 800 fps | Linux | VA-API copy-out through WSL's d3d12 driver: 118 to 128 fps (`wl-video`: 120 to 124) |
| Speed check: AV1 software decode over 400 fps | Linux | dav1d 357 fps in WSL (`wl-video`: 366) |
| Speed check: 10-bit HEVC over 800 fps as a whole program | Windows | 429 fps; a Mac limit that the program start alone exceeds (`wl-video`) |

What the merge changed in the results: the d3d12 scaling-matrix clip of `61_vaapi` now decodes on FFamp's own decoder, every frame
bit-exact (OpenH264 gave 71.9 dB); `54_h264`, `53_ac3`, `54_aac`, `53_h263`, `60_videox` and `61_vaapi` pass whole on both systems.

## Speed against FFmpeg

`tests/speed.py` on the merged build, nothing else running on the PC, the suite's 60 s 1080p clips; FFmpeg is the BtbN 9.0.2 build the
app bundles. The factor is FFmpeg's time over FFamp's (above 1: FFamp is faster).

Windows, default paths (Media Foundation on the GPU for H.264 and HEVC, the GPU encoder for H.264 export):

| Task | FFamp | FFmpeg | Factor |
|---|---|---|---|
| open + probe, H.264 / HEVC clip | 13 / 13 ms | 72 / 32 ms | 5.4x / 2.4x |
| frame index, H.264 / HEVC | 96 / 73 ms | 128 / 82 ms | 1.34x / 1.13x |
| 12 thumbnails, H.264 / HEVC | 2.17 / 3.62 s | 1.89 / 3.57 s | 0.87x / 0.99x |
| decode to memory, H.264 | 1.71 s | 1.46 s (software), 2.45 s (`-hwaccel d3d11va`) | 0.86x / 1.44x |
| decode to memory, HEVC | 2.00 s | 2.67 s (software), 2.64 s (d3d11va) | 1.33x / 1.32x |
| trim 20 s from 10 s, H.264 / HEVC | 38 / 35 ms | 101 / 60 ms | 2.7x / 1.7x |
| extract audio by copy, H.264 / HEVC clip | 26 / 26 ms | 99 / 55 ms | 3.8x / 2.1x |
| extract audio to MP3 128 kbit/s | 348 ms | 309 ms | 0.89x |
| export MP4, 300 frames 1080p H.264 8 Mbit/s + AAC 128 kbit/s | 1,043 ms | 827 ms (`h264_amf`) | 0.79x |

Linux (WSL), default paths (VA-API through Mesa's d3d12 driver for H.264 and HEVC):

| Task | FFamp | FFmpeg | Factor |
|---|---|---|---|
| open + probe, H.264 / HEVC clip | 2 / 2 ms | 55 / 12 ms | 29x / 8.0x |
| frame index, H.264 / HEVC | 64 / 44 ms | 65 / 20 ms | 1.01x / 0.45x |
| 12 thumbnails, H.264 / HEVC (VA-API) | 3.79 / 5.15 s | 2.14 / 4.04 s | 0.56x / 0.78x |
| decode to memory, H.264 / HEVC (VA-API) | 8.89 / 10.2 s | 1.57 / 2.89 s | 0.18x / 0.28x |
| trim 20 s from 10 s, H.264 / HEVC | 21 / 20 ms | 83 / 37 ms | 3.9x / 1.8x |
| extract audio by copy, H.264 / HEVC clip | 6 / 7 ms | 83 / 37 ms | 13x / 5.4x |
| extract audio to MP3 128 kbit/s | 279 ms | 385 ms | 1.38x |
| export MP4, 300 frames 1080p H.264 8 Mbit/s + AAC 128 kbit/s | 1.40 s | 1.39 s (`libx264` veryfast, `aac`) | 0.99x |

H.264 through FFamp's own decoder (frame threads, assembly), on Windows with `FFAMP_NO_MEDIA_FOUNDATION=1` and on Linux with
`FFAMP_NO_VAAPI=1`, and as `speed.py --libraries` runs it (`--software`):

| Task (H.264 clip) | Windows FFamp | FFmpeg | Factor | Linux FFamp | FFmpeg | Factor |
|---|---|---|---|---|---|---|
| decode to memory | 1.18 s | 1.45 s | 1.23x | 1.34 s | 1.52 s | 1.13x |
| decode to memory, `--software` | 1.17 s | 1.44 s | 1.22x | 1.33 s | 1.56 s | 1.17x |
| 12 thumbnails (a program start each) | 0.74 s | 1.86 s | 2.53x | 0.88 s | 2.15 s | 2.45x |
| decode against FFmpeg's `-hwaccel d3d11va` | 1.18 s | 2.59 s | 2.20x | | | |

The other software libraries (`speed.py --libraries`, Media Foundation and VA-API off):

| Library task (1080p) | Windows FFamp / FFmpeg | Linux FFamp / FFmpeg |
|---|---|---|
| HEVC decode, 60 s (libde265) | 13.0 / 2.69 s (0.21x) | 10.9 / 2.88 s (0.26x) |
| VP9 decode, 60 s (libvpx) | 2.61 / 2.24 s (0.86x) | 3.42 / 2.36 s (0.69x) |
| AV1 decode, 60 s (dav1d) | 1.82 / 2.07 s (1.14x) | 2.56 / 3.39 s (1.32x) |
| H.264 encode, 300 frames (x264) | 0.73 / 0.75 s (1.02x) | 1.42 / 1.42 s (1.00x) |
| HEVC encode, 300 frames (x265) | 4.03 / 3.61 s (0.90x) | 4.01 / 3.73 s (0.93x) |
| AV1 encode, 300 frames (SVT-AV1) | 1.09 / 1.08 s (0.99x) | 1.09 / 1.13 s (1.04x) |
| VP9 encode, 300 frames (libvpx) | 1.36 / 1.39 s (1.02x) | 1.77 / 1.96 s (1.11x) |

What the numbers say:

- FFamp's own H.264 decoder beats FFmpeg's software decoder on both systems (1.13x to 1.23x) and FFmpeg's D3D11VA path by 2.2x; with
  it, 12 thumbnails take less than half of FFmpeg's time on both systems.
- The default H.264 paths are slower than that decoder: Media Foundation on the GPU decodes at 0.86x FFmpeg (the GPU's own limit,
  `wl-video`), and VA-API under WSL at 0.18x, because every picture comes out through the d3d12 driver at about 200 fps. Which
  decoder comes first is left as it was: Media Foundation on Windows (the owner has not decided), VA-API on Linux (`wl-video`'s
  choice; native Intel and AMD drivers, where the copy-out is not paravirtualised, were not available to measure).
- HEVC has no fast software decoder yet (libde265, 0.21x to 0.26x); the GPU paths cover it on Windows (1.33x).

# Windows and Linux follow-up: own H.264 first on Windows, 6.1 in FFmpeg's order, FATE h264 conformance

Owner decisions of 2026-11-16, on branch `wl-followup`. Same PC and judge as before (Windows 11 and Ubuntu 24.04 under WSL, the BtbN
FFmpeg 9.0.2 the app bundles).

## FATE h264 conformance

FFmpeg's FATE suite folder `h264-conformance` (rsync://fate-suite.ffmpeg.org/fate-suite/h264-conformance/, the ITU-T H.264.1 / JVT
bitstreams), fetched with the owner's go onto the Windows PC and into WSL only: 197 files, 139,889,402 bytes. FFmpeg's
`tests/fate/h264.mak` uses 185 of them (`tests/h264_fate.txt`); each was decoded by FFamp's own decoder with one thread, frame threads
and slice threads and compared frame by frame with `ffmpeg -threads 1 -f framemd5` of the same bitstream.

| Group | Files | Windows (clang-cl, NASM) | Linux x86-64 (gcc, NASM) |
|---|---|---|---|
| Baseline and Extended profile | 32 | 32 pass | 32 pass |
| Main profile CAVLC, progressive | 10 | 10 pass | 10 pass |
| Main profile CABAC, progressive | 30 | 30 pass | 30 pass |
| Interlaced (field pictures, PAFF, MBAFF) | 44 | 44 pass | 44 pass |
| Multiple reference frames and MMCO | 11 | 11 pass | 11 pass |
| FRExt High profile, 8 bits | 42 | 42 pass | 42 pass |
| FRExt High 10 and High 4:2:2 | 16 | 16 pass | 16 pass |

The first run passed 156 of the 185: 29 files (Toshiba, Sony, HHI, Tandberg, among them most of the interlaced ones) came out 2 to 9
pictures short at the start. Those streams have B-pictures and no VUI reorder count, and some show pictures before their first key
picture. FFmpeg's decoder drops such pictures as out of order too when it starts with a reorder delay of 0, but the ffmpeg program
never starts there: its stream probe decodes the first 7 to 20 pictures and hands the reorder delay it found to the real decoder.
FFamp's decoder now does the same (`ffamp_h264_guess_delay`, CONVENTIONS), in `ffamp h264` and in `VideoDecoder`, and all 185 pass on
both systems. Streams with a reorder count (every x264 file) are not decoded twice.

The suite (`54_h264`) runs the seven groups when `H264_CONFORMANCE` (or `FATE_SAMPLES`) points at the folder and skips them otherwise;
the bitstreams are not in the repository.

## 6.1 sound off the Mac

FFamp's own ALAC and AAC decoders already gave 6.1 in FFmpeg's order (FL FR FC LFE BL BR BC, FFmpeg's `6.1(back)`), which is what
`54_aac` asked; `fixes: channel-order` asked for the Mac's WAVE 6.1 order (FL FR FC LFE BC SL SR). Off the Mac FFmpeg's order is now the
rule (the check follows it, and the fixes suite judges both 6.1 fixtures against ffmpeg there); the Mac is unchanged. Media Foundation's
AAC decoder refuses 6.1 and 7.1, so FFamp's own decoder takes them on Windows too.

The order was not the whole problem: FFamp's mixer placed 7 channels by count as FL FR FC LFE BC SL SR, so with FFmpeg's order a 6.1
file mixed down to stereo with the left back channel in the middle, the right back channel on the left and the back centre on the
right. The decoders now state their FFmpeg layout and the mixer places each channel by it; the 6.1 ALAC fixture mixes to stereo at
0.986 / 1.157 peak, ffmpeg's `-ac 2` gives 0.988 / 1.159, the Mac 0.986 / 1.157. AAC with other FFmpeg layouts (2.1, 3.0(back),
hexagonal, 7.1(wide) through a program config element) is mixed by its layout the same way.

How the app takes it (read in the app's 5.2 tree and in the Windows/Linux switch branch): the desktop sound path asks FFamp's
`PcmReader` for the file's own channel count, so no mixing happens in FFamp, and mixes to stereo itself with `downmixGains(channels, 0)`,
which places 7 channels as Android's 5.1 + back centre: FL FR FC LFE BL BR BC. That is FFmpeg's order for 6.1 ALAC and AAC, so each
channel lands on its own speaker. The viewer's playback asks `PcmReader` for 2 channels, which goes through FFamp's mixer and now places
them by layout as well. For comparison, the app's FFmpeg path today (`-ac 7`) turns a 6.1(back) file into FFmpeg's 6.1 with both back
channels folded into the back centre and silent side channels, which the app's mixdown then puts on the left only. Two things outside
this change: on the Mac (AudioToolbox's WAVE 6.1 order) and for 6.1 files whose layout really is FL FR FC LFE BC SL SR (7-channel
FLAC, WAV with mask 0x70F) the app's count-based mixdown puts the back centre on the left, the left side channel on the right and the
right side channel in the middle, with FFmpeg as with FFamp; the app would need the layout (or to ask FFamp for 2 channels) to place
those.

## Windows: FFamp's own H.264 decoder first

`tests/speed.py` on the idle PC, the suite's 60 s 1080p H.264 clip; "Media Foundation" is the same build with `FFAMP_SYSTEM_H264=1`
(the order before this round). The factor is FFmpeg's time over FFamp's.

| Task (H.264 1080p, 60 s) | Own decoder first (now) | Media Foundation first (before) | FFmpeg software | FFmpeg `-hwaccel d3d11va` |
|---|---|---|---|---|
| decode to memory | 1.18 s (1.22x) | 1.70 s (0.84x) | 1.43 s | 2.56 s |
| 12 thumbnails, a program start each | 0.70 s (2.64x) | 2.05 s (0.92x) | 1.86 s | |
| 50 random frames in one program (`10_video` seek, all exact) | 35 ms per frame (median 34) | 32 ms (median 33) | 145 ms per jump (a program each, `cliseek`) | |

The other rows of the merged table (probe, index, trim, audio, export, HEVC) did not move. The thumbnails gain most: Media Foundation
starts a D3D11 device and its decoder in every program, the own decoder starts in a few milliseconds. Random jumps inside one program
are 3 ms slower on the own decoder, whose frame threads need a few more packets before the first picture of a jump comes out.

Real time and processor use (`tests/speed.py --realtime`): 20 s clips made by x264 veryfast with B-frames, testsrc2 with noise at
15, 45 and 80 Mbit/s, decoded to memory as fast as possible. "CPU while playing" is the process's whole processor time over the 20
seconds of the clip, what decoding at the clip's own speed costs (the PC has 8 cores, 16 threads).

| Clip | Decoder | Frames per second | Against real time | CPU while playing |
|---|---|---|---|---|
| 1080p60, 15 Mbit/s | FFamp, own decoder | 1,483 | 24.7x | 0.33 cores (2 %) |
| | FFamp, Media Foundation on the GPU | 1,039 | 17.3x | 0.13 cores (1 %) |
| | FFmpeg software | 1,261 | 21.0x | 0.46 cores (3 %) |
| | FFmpeg `-hwaccel d3d11va` | 684 | 11.4x | 0.21 cores (1 %) |
| 4K30, 45 Mbit/s | FFamp, own decoder | 248 | 8.3x | 0.84 cores (5 %) |
| | FFamp, Media Foundation on the GPU | 286 | 9.5x | 0.51 cores (3 %) |
| | FFmpeg software | 234 | 7.8x | 1.03 cores (6 %) |
| | FFmpeg `-hwaccel d3d11va` | 188 | 6.3x | 0.43 cores (3 %) |
| 4K60, 80 Mbit/s | FFamp, own decoder | 294 | 4.9x | 1.81 cores (11 %) |
| | FFamp, Media Foundation on the GPU | 298 | 5.0x | 1.08 cores (7 %) |
| | FFmpeg software | 253 | 4.2x | 2.20 cores (14 %) |
| | FFmpeg `-hwaccel d3d11va` | 190 | 3.2x | 1.05 cores (7 %) |

Every clip keeps real time on the own decoder with a wide margin, 4K60 included (4.9 times faster than real time), so no rule for
handing 4K60 to Media Foundation is needed on this PC. The price is processor time: playing 4K60 costs about 1.8 cores on the own
decoder against about 1.1 with Media Foundation (whose processor time is the copy back from the GPU and FFamp's own work); at 4K30 and
1080p60 the difference is under half a core. The own decoder still uses less processor time than FFmpeg's software decoder everywhere.
At 4K, Media Foundation is the faster of the two by up to 15 % (the GPU's decoder), at 1080p the own decoder is 43 % faster.

## Linking next to FFmpeg, encoder names

The app links FFamp into the same native library as FFmpeg on every desktop. The H.264 port defined about 700 functions and tables
under FFmpeg's own names (`av_frame_alloc`, `ff_h264_decode_init`, the assembly's `ff_*` functions), and FFamp's `av_*` functions
stood in for FFmpeg's in the app's FFmpeg preview decoder (a double free on Linux). Every such name now carries the `ffamp_` prefix
(CONVENTIONS); `nm` finds no `av_`, `avpriv_` or `ff_` symbol in the Windows, Linux or Mac (arm64, NEON) library, and the H.264 suite
checks it. `encoders()` now names `h264_mf`, `hevc_mf`, `aac_mf` and `aac` on Windows and `libx264`, `libx265` and `aac` on Linux,
the names the app's export chooses from; `fixes: encoder-names` checks them and runs each listed video encoder's trial.

## Suites

Full runs of `tests/run.sh` (pass / fail / skip), with the conformance set present:

| System | This round | Merged round 2 |
|---|---|---|
| Windows (clang-cl, libraries with NASM) | 3,105 / 5 / 222 | 3,087 / 6 / 223 |
| Linux (WSL, gcc 13, VA-API through d3d12) | 3,260 / 7 / 201 | 3,248 / 8 / 202 |

No new failure. `fixes: channel-order` passes on both systems now. The new checks: the seven conformance groups (they replace one
skip), own decoder first on Windows and Media Foundation first with `FFAMP_SYSTEM_H264` (backend, and every frame and 50 random frames
of the 720p and 1080p clips bit-exact through Media Foundation, so the fallback stays tested), the symbol check, the two 6.1 orders
against ffmpeg and the encoder names. What still fails is what failed in the merged run: `rd_vorbis.ogg`'s trim (both), the WMA 2
600 s speed checks and the 10-bit HEVC whole-program rate (Windows), `wv_speed_v3.avi`'s one-thread speed (both), and on Linux the
100-frame AAC file (vo-aacenc's priming), the RGBA frame rates through WSL's d3d12 copy-out and dav1d's 400 fps.

# Windows platform fixes: what Media Foundation cannot decode, HE-AAC timing, paths and open files

Six findings of the platform review, on branch `fix-platform-517`. Same PC and judge as the follow-up round (Windows 11 and Ubuntu 24.04
under WSL, the BtbN FFmpeg 9.0.2 the app bundles); the Mac runs the suites its code reaches.

| Finding (Windows) | Before | Now |
|---|---|---|
| HEVC 4:2:2 (8 and 10-bit) and 4:4:4 | Microsoft's decoder opens them and waits forever in its drain (no processor time); without the GPU, no picture | libde265, every frame bit-exact against ffmpeg |
| HEVC 12-bit 4:2:0, 8192 x 8192 | open on Media Foundation, then `frame 0 did not decode` | libde265, bit-exact; 8192 x 4320 (within level 6.2) stays on the GPU, bit-exact |
| HE-AAC in transport streams (implicit SBR) | 38,912 of 77,824 frames: the second half missing | 77,824 frames, 127 to 131 dB against ffmpeg at no offset |
| HE-AAC in ADTS `.aac` | right length, 2,048 frames late, the last 46 ms cut | no offset, 101 to 131 dB |
| LC AAC at 16 and 22.05 kHz (not in the finding, same cause) | silence: Media Foundation guesses SBR, opens at twice the rate, then changes its output format and FFamp stopped reading | FFamp's own decoder, 98 dB against ffmpeg at no offset |
| AAC with a program config element (2.1, 3.0(back), quad, hexagonal) | 2.1 silent; the others in Media Foundation's element order, mixed by count | FFamp's own decoder: stereo mix 138 to 139 dB against `ffmpeg -ac 2` |
| MPEG-2 4:2:2 (plain, XDCAM HD422-style 1080i, IMX) | `frame 0 did not decode` | libmpeg2 (C code, built for Windows now): 65.5 to 66.1 dB against ffmpeg's IDCT |
| MPEG-1/2 without Media Foundation (N editions) | `not decoded on this system yet` | libmpeg2, 65.5 to 66.2 dB |
| 1,000 open readers in one process | the 510th fails (`cannot open file`), then every `fopen` of the process | all 1,000 open, files still written |
| `ffamp` with paths in Café, Привет, 動画 | `cannot open file` for every command | probe, extract-audio (copy to MP3 through a renamed `.part` file, and to M4A) work |

How: `openPlatformVideo` on Windows takes HEVC only as Main or Main 10 at 4:2:0 within level 6.2 (the check VA-API already made) and
MPEG-1/2 only at 4:2:0 (the chroma format of the first sample's sequence extension); everything else falls to the software decoders,
as on Linux. Media Foundation AAC refuses channel configuration 0, sets its frames per packet and delay from its own output rate, handles
a format change, and gives the stream to FFamp's own decoder when its implicit-SBR guess proves wrong on the first packet. `openFile` raises
the C runtime's stream limit to 8,192; the command rebuilds its arguments from `GetCommandLineW` as UTF-8 and opens, removes and renames
files through the UTF-8 helpers. libmpeg2 0.5.1 is built by `build_windows.sh` with MSVC (C code; 135,684 bytes).

New checks: `54_aac` runs the HE-AAC, low-rate and program config element files through the default decoder (Media Foundation on
Windows) before it turns the system decoder off; `60_videox` checks HEVC 4:2:2, 4:4:4, 12-bit and 8192 x 8192 on libde265 bit-exact,
4:2:2 MPEG-2 on libmpeg2 and MPEG-1/2 without Media Foundation, on Windows and Linux, each open under a 60 s limit; `fixes: many-readers`
(Windows) keeps 1,000 readers open, and the fixes suite opens, probes and extracts from folders and files named Café, Привет and 動画.

Suites the change reaches (pass / fail / skip):

| Suite | Windows (clang-cl) | Linux (WSL, gcc 13) | Mac |
|---|---|---|---|
| `88_fixes` | 53 / 0 / 0 | 51 / 1 / 0 | 51 / 0 / 1 |
| `54_aac` | 98 / 0 / 0 | 98 / 0 / 0 | skipped (AudioToolbox) |
| `60_videox` | 236 / 0 / 7 | 316 / 0 / 6 | not reached |
| `45_formats` | 341 / 0 / 30 | 353 / 0 / 18 | not reached |
| `20_audio` | 163 / 0 / 2 | 163 / 0 / 2 | not reached |
| `70_tools` | 127 / 0 / 21 | 130 / 0 / 20 | 150 / 0 / 0 |
| `50_writers` | 369 / 0 / 67 | 369 / 0 / 67 | 436 / 0 / 0 |

The one failure is the Linux 100-frame AAC file of the earlier rounds (vo-aacenc's priming), unchanged.
