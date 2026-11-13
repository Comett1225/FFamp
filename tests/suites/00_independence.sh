echo "== independence: no FFmpeg inside"
no_ffmpeg_libraries() { ! otool -L ../build/amedia | grep -Eiq 'libav(codec|format|util|filter|device)|libsw(scale|resample)|ffmpeg'; }
no_ffmpeg_mentions() { ! strings ../build/amedia ../build/libabsentmedia.a | grep -Eiq 'ffmpeg|ffprobe|libavcodec'; }
check "amedia links no FFmpeg library" no_ffmpeg_libraries
check "amedia and the library never mention ffmpeg or ffprobe" no_ffmpeg_mentions
