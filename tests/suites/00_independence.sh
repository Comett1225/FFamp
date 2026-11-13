echo "== independence: no FFmpeg inside"
no_ffmpeg_libraries() { ! otool -L ../build/ffamp | grep -Eiq 'libav(codec|format|util|filter|device)|libsw(scale|resample)|ffmpeg'; }
no_ffmpeg_mentions() { ! strings ../build/ffamp ../build/libffamp.a | grep -Eiq 'ffmpeg|ffprobe|libavcodec'; }
check "ffamp links no FFmpeg library" no_ffmpeg_libraries
check "ffamp and the library never mention ffmpeg or ffprobe" no_ffmpeg_mentions
