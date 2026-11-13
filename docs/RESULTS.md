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
absent-media, and every real FFmpeg program and library on the machine denied by a sandbox for the whole process tree.

| Check | Result |
|---|---|
| FFmpeg command lines the app runs for one import and export | 10 calls of 9 kinds; all answered by the stand-ins, none refused |
| Real FFmpeg programs and libraries tried from inside the sandbox | 36 of 36 blocked |
| Frames, length | 811 and 13.5167 s on both paths |
| Checksum of every rendered frame, FFmpeg decoder against absent-media | 811 of 811 identical |
| The app's audio mix (clip sound decoded by FFmpeg against absent-media) | same length to the sample (596,055); 99.6 % of sample values identical, largest difference 2 of 32,768, 110 dB |
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
