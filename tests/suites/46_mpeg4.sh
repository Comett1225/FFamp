echo "== MPEG-4 part 2 through Xvid: B-frames, quarter-pixel, GMC, matrices, interlaced, packed AVI (no FFmpeg)"
PX=fixtures/asp
PX_LAVC="lavc_qpel.mp4 lavc_qpel.mkv lavc_qpel.avi lavc_packed.avi lavc_interlaced.mp4 lavc_fields.mkv lavc_matrix.mkv"
PX_XVID="xvid_gmc.avi xvid_interlaced.avi"
if ! $A videoinfo $PX/lavc_qpel.mp4 2> /dev/null | grep -q "backend xvid"; then
    skip "MPEG-4 part 2 through Xvid (built without it)"
    return 0
fi
px_listed() { [ "$("${@:3}" 2> /dev/null | md5)" = "$(grep " $2\$" $PX/$1.md5 | cut -d' ' -f1)" ]; }
px_frames() { [ "$($A videoinfo $PX/$1 | sed 's/.* frames \([0-9]*\) .*/\1/')" = "$2" ]; }
px_same() { [ "$($A decode $PX/$1 | md5)$($A decode $PX/$1 --times | md5)" = "$($A decode $PX/$2 | md5)$($A decode $PX/$2 --times | md5)" ]; }
px_threads() { [ "$($A decode $PX/$1 --threads 10 | md5)" = "$($A decode $PX/$1 | md5)" ]; }
px_leaks() { leaks --atExit -- $A frames $PX/$1 --list 29,3,17,0,12,28,1 2> /dev/null | LC_ALL=C grep -aq ": 0 leaks for 0 total leaked bytes"; }
for f in $PX_LAVC $PX_XVID; do
    check "$f pictures decode to the checked checksum" px_listed pictures $f $A decode $PX/$f
    check "packets of $f match the checked table" px_listed packets $f $A packets $PX/$f
    check "$f has 30 frames" px_frames $f 30
    check "random frames of $f equal the straight decode" python3 comparex.py selfseek $PX/$f 16 5
done
for f in lavc_qpel.mkv lavc_qpel.avi lavc_packed.avi; do check "$f gives the pictures and frame times of the same stream in MP4" px_same $f lavc_qpel.mp4; done
check "the DivX packed AVI has the packet times of the same stream in MP4" python3 formats_check.py times $PX/lavc_packed.avi $PX/lavc_qpel.mp4
for f in xvid_gmc.avi lavc_packed.avi; do check "ten threads decoding $f at once each get the straight decode" px_threads $f; done
if command -v leaks > /dev/null; then
    for f in xvid_gmc.avi lavc_qpel.mkv; do check "jumping around $f leaks nothing" px_leaks $f; done
fi
PX_FUZZ=20
[ -n "${QUICK:-}" ] && PX_FUZZ=6
for f in lavc_packed.avi lavc_qpel.mkv xvid_gmc.avi xvid_interlaced.avi; do check "damaged copies of $f decode or fail cleanly" python3 comparex.py fuzz $PX/$f $PX_FUZZ 9; done

have_ffmpeg || { skip "MPEG-4 part 2 against ffmpeg"; return 0; }

echo "== MPEG-4 part 2 through Xvid against ffmpeg's decode"
for f in $PX_LAVC; do
    [ $f = lavc_packed.avi ] && continue
    check "decode $f bit-exact against ffmpeg with Xvid's IDCT" python3 comparex.py decode $PX/$f --xvid-idct
    check "decode $f against ffmpeg's own IDCT" python3 comparex.py decode $PX/$f 55
    check "random frames of $f against ffmpeg with Xvid's IDCT" python3 comparex.py seek $PX/$f 12 7 --xvid-idct
done
check "decode xvid_interlaced.avi bit-exact against ffmpeg" python3 comparex.py decode $PX/xvid_interlaced.avi
check "decode xvid_gmc.avi against ffmpeg (GMC rounds a few pixels differently)" python3 comparex.py decode $PX/xvid_gmc.avi 80
check "random frames of xvid_interlaced.avi against ffmpeg" python3 comparex.py seek $PX/xvid_interlaced.avi 12 7
for f in lavc_qpel.mp4 lavc_qpel.mkv lavc_interlaced.mp4 lavc_fields.mkv lavc_matrix.mkv; do
    check "frame times of $f equal ffprobe's" python3 formats_check.py frametimes $PX/$f
    check "fields of $f" python3 probe_check.py compare $PX/$f stream0.bit_rate stream0.field_order stream0.color_range
done
