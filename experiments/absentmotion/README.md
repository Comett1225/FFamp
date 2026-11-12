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

## What the first run showed

One 13.5 second, 1080 x 1080, 60 fps project with three H.264 clips and about 790 elements:

- all 811 rendered frames are byte-identical with either decoder, and all 3,251 frames of the three clips decode identically;
- whole pipeline 2,452 ms with absent-media against 2,839 ms with FFmpeg; rendering with decoding 1,468 ms against 1,838 ms;
  encoding and writing alone are the same speed;
- both files are clean in ffprobe and in AVFoundation, with the sound at offset zero against the app's mix;
- the only picture difference comes from who converts RGB to the video colour format: FFmpeg's scaler or Apple's encoder.
