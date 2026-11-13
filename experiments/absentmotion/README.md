# Experiment: one real AbsentMotion export through both pipelines

This folder holds the pieces used to export one real project twice on macOS, once the way the desktop app does it today (FFmpeg) and
once with absent-media, and to compare the results. Nothing here is part of the library, and nothing is written into the app's
source tree.

## Pieces

- `video_absent.mm`: a stand-in for the app's `pc/native/video.cpp` and `macos/native/video_mac.mm`. It implements the same five JNI
  functions on top of `absent::Mp4Reader` and `absent::VideoDecoder` and hands the decoded frames to Skia as Metal textures. It covers
  8-bit H.264 and HEVC in MP4 and MOV, on the Metal back end only.
- `build_variant.sh <output folder>`: compiles the stand-in with the flags of the app's existing native build tree and links a second
  `libamdesktop.dylib` from the app's already built object files, without the two FFmpeg-based files and without any FFmpeg library.
  Needs `APP_REPO` set to the app's source folder, an existing `pc/build/native-mac` build tree there, and `build/libabsentmedia.a` here.
- `harness/`: a JUnit class in the app's `export` package. It imports a project with the app's importer and then, depending on
  `FM_MODE`, runs the app's real export (`app`), the same frame loop feeding ffmpeg with the app's arguments (`stages-ffmpeg`), the same
  loop feeding `amedia encode` (`stages-absent`), a render-only pass that writes a checksum per frame (`render`), or a raw dump of
  every frame (`dump`).
- `harness.init.gradle`: adds `harness/` to the app's test sources for one Gradle run, so no file has to be placed in the app's tree.
- `run_harness.sh`: runs that one test class. Needs `APP_REPO`.
- `shim/shim.cpp`: two stand-in programs, built as `build/shim/ffmpeg` and `build/shim/ffprobe` with `cmake -DABSENT_SHIM=ON` (off by default; they are
  not part of the library or of `amedia`). They accept exactly the command lines the desktop app runs while it imports and exports a project and do the
  work with absent-media. Any other command line is printed, logged and refused with exit code 64, so nothing falls back silently. With
  `ABSENT_SHIM_LOG` set, every call is appended to that file with its process id, its own path, its arguments and its exit code.
- `run_zero.sh <work folder>`: runs the harness with the stand-ins in place of FFmpeg and with every real FFmpeg file out of reach: `ffmpeg` and
  `ffprobe` are not on `PATH`, and a sandbox profile denies reading, running and loading the folders that hold the real programs and libraries (found
  through `PATH`, in the app's build folders, plus `FM_HIDE`). The app's usual engine library, which links FFmpeg, may be looked at (Gradle checks that
  the file exists) but not loaded. Before the run it tries each real file from inside the sandbox and writes the result to `blocked-<tag>.txt`.

## Settings (environment)

| Name | Meaning |
|---|---|
| `FM_MODE` | `app`, `stages-ffmpeg`, `stages-absent`, `render`, `dump` |
| `FM_PROJECT` | the `.absproj` file |
| `FM_WORK` | a scratch folder for results |
| `FM_RUNS` | how many times to run; the first is the warm-up |
| `FM_TAG` | a name for the result files |
| `FM_NATIVE` | the engine library to load; point it at the variant to swap the decoder |
| `FM_FFMPEG`, `FM_FFPROBE` | the FFmpeg binaries the app should use |
| `FM_DUMP` | where `dump` writes the raw frames (about 3.8 GB for 811 frames of 1080 x 1080) |
| `FM_KEEP` | keep the audio mix file after a run |

After a run, delete the harness's class files and test report from the app's `pc/build` folder.

## The command lines the stand-ins know

Logged from one real import and export of the test project (app code unchanged):

| Program | Command line | Done with |
|---|---|---|
| ffprobe | `-v error -show_streams -show_format -of json FILE` | `probeJson` |
| ffprobe | `-v error -show_entries stream=codec_type,codec_name,duration:stream_tags=DURATION:format=duration -of json FILE` | `probeJson` (the whole description; the asked entries are in it) |
| ffprobe | `-v error -select_streams v:0 -read_intervals %+#1 -show_entries frame=pts_time -of csv=p=0 FILE` | `VideoDecoder`: decodes the first frame and prints its time |
| ffprobe | `-v error -select_streams v:0 -read_intervals T% -show_entries packet=pts_time -of csv=p=0 FILE` | `Reader`: packet times from the key frame at or before T |
| ffprobe | the same with `frame=pts_time` | `VideoDecoder`: decodes every frame from that key frame to the end |
| ffmpeg | `-hide_banner -encoders` | a fixed list: `h264_videotoolbox`, `aac_at` |
| ffmpeg | `-nostdin -v error [-ss A] -i FILE [-ss B] -map 0:a:0 -vn -sn -dn -af aresample=async=1:first_pts=0 -ar R -ac C -f f32le -` | `PcmReader` from A + B seconds on the file's clock, silence before the sound starts |
| ffmpeg | `-v error -f lavfi -i color=c=black:s=WxH:r=F:d=0.2 -vf scale=... -c:v h264_videotoolbox ... -frames:v 2 -f null -` | `VideoEncoder`: two black frames |
| ffmpeg | `-hide_banner -v error -f rawvideo -pix_fmt rgb0 -s WxH -r F -i - -f s16le -ar R -ac 2 -i PCM -map 0:v -vf scale=... -c:v h264_videotoolbox -realtime 0 -profile:v high -b:v B -g G -pix_fmt yuv420p -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv -map 1:a -c:a aac_at -b:a Kk -ar R -ac 2 -f mp4 -movflags +faststart -y OUT.mp4` | the `encode` command: VideoToolbox H.264, AudioToolbox AAC, own MP4 writer |

Where the stand-in's sound differs from FFmpeg's on purpose (measured in `tests/suites/86_shim.sh` and by hand): HE-AAC comes out 962 samples earlier
(FFmpeg leaves the SBR delay in), Opus in MP4 ends at the length the file states (FFmpeg adds the encoder's padding), a seek before the input in
Matroska or 5.1 AAC lands on the asked sample (FFmpeg lands up to 1 ms away), and a file with a gap in its sound keeps the gap as silence.

## What the first run showed

One 13.5 second, 1080 x 1080, 60 fps project with three H.264 clips and about 790 elements:

- all 811 rendered frames are byte-identical with either decoder, and all 3,251 frames of the three clips decode identically;
- whole pipeline 2,452 ms with absent-media against 2,839 ms with FFmpeg; rendering with decoding 1,468 ms against 1,838 ms;
  encoding and writing alone are the same speed;
- both files are clean in ffprobe and in AVFoundation, with the sound at offset zero against the app's mix;
- the only picture difference comes from who converts RGB to the video colour format: FFmpeg's scaler or Apple's encoder.
