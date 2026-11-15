echo "== independence: no FFmpeg inside"
no_ffmpeg_libraries() { ! { if on_mac; then otool -L $A; else ldd $A; fi; } | grep -Eiq 'libav(codec|format|util|filter|device)|libsw(scale|resample)|ffmpeg'; }
no_ffmpeg_mentions() { ! strings $A ../build/libffamp.a ../build/libffamp.lib 2> /dev/null | grep -v 'broken ffmpeg default settings' | grep -Eiq 'ffmpeg|ffprobe|libavcodec'; }
check "ffamp links no FFmpeg library" no_ffmpeg_libraries
check "ffamp and the library never mention ffmpeg or ffprobe" no_ffmpeg_mentions
