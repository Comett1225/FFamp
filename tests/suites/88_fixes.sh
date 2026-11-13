echo "== fixes: one check per reviewed finding, made and read back by amedia itself (no FFmpeg), then FFmpeg as the judge when present"
FX=$O/fixes
rm -rf $FX
mkdir -p $FX
for name in $($A fixcheck); do check "fixes: $name" $A fixcheck $name $FX fixtures; done
if have_ffmpeg; then
    decoded_bytes() { [ "$($FFMPEG -nostdin -v error -i "$1" -f s16le - | wc -c | tr -d ' ')" = "$2" ]; }
    stated_length() { [ "$($FFPROBE -v error -show_entries format=duration -of csv=p=0 "$1" 2> /dev/null)" = "$2" ]; }
    loudness_order() { python3 -c 'import sys, numpy; d = numpy.frombuffer(sys.stdin.buffer.read(), "<f4"); n = int(sys.argv[1]); f = d[:len(d) // n * n].reshape(-1, n); print(*numpy.argsort(numpy.abs(f[len(f) // 4:len(f) * 3 // 4]).max(axis=0)))' "$1"; }
    same_levels() { [ "$($A adecode "$1" 2> /dev/null | loudness_order "$2")" = "$($FFMPEG -nostdin -v error -i "$1" -f f32le - | loudness_order "$2")" ]; }
    check "fixes: FFmpeg decodes nothing from the empty AAC file" decoded_bytes $FX/empty.m4a 0
    check "fixes: FFmpeg decodes nothing from the empty Opus file" decoded_bytes $FX/empty.opus 0
    check "fixes: FFmpeg decodes 100 frames from the 100-frame AAC file" decoded_bytes $FX/short.m4a 400
    check "fixes: FFmpeg decodes 100 frames from the 100-frame Opus file" decoded_bytes $FX/short.opus 400
    check "fixes: FFmpeg reads the 13 hour length of the long MP4" stated_length $FX/long.m4a 47722.858844
    check "fixes: FFmpeg reads the late trim of the PCM track in MP4" stated_length $FX/trimmed.mp4 1.000000
    check "fixes: FFmpeg reads the late trim of the PCM track in Matroska" stated_length $FX/late.mka 0.052083
    check "fixes: 7.1 AAC channels come out in FFmpeg's order" same_levels fixtures/fixes/aac_71.m4a 8
else
    skip "fixes: FFmpeg as the judge of the written files and the 7.1 order"
fi
rm -rf $FX
