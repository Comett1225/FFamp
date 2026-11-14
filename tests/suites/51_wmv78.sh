echo "== Microsoft MPEG-4 v1 to v3, WMV7 and WMV8: committed fixtures made by fixtures/wmv78/make.sh and fixtures/asf/make.sh (no FFmpeg)"
WV=fixtures/wmv78
WK="python3 vc1_check.py"
wv_listed() { [ "$($A decode "$2" 2> /dev/null | md5)" = "$(grep " $1\$" $WV/pictures.md5 | cut -d' ' -f1)" ]; }
wv_backend() { $A videoinfo "$1" | grep -q "backend $2 "; }
wv_keys() { [ "$($A packets "$1" | grep -c ' K ')" = "$2" ]; }
wv_threads() { [ "$($A decode "$1" --threads 6 | md5)" = "$($A decode "$1" | md5)" ]; }
WV_FUZZ=12
[ -n "${QUICK:-}" ] && WV_FUZZ=4
while read f backend; do
    check "$(basename $f) decodes with FFamp's own $backend decoder" wv_backend $f $backend
    check "$(basename $f) pictures decode to the checked checksum" wv_listed $(basename $f) $f
    check "random frames of $(basename $f) equal the straight decode" python3 comparex.py selfseek $f 8 5
done <<TABLE
$WV/v2_odd.avi msmpeg4v2
$WV/v3_q2.avi msmpeg4v3
$WV/v3_noise.mkv msmpeg4v3
$WV/wmv1_noise.wmv wmv1
$WV/wmv2_loop.mkv wmv2
$WV/wmv2_q31.avi wmv2
fixtures/asf/msmpeg4v2.asf msmpeg4v2
fixtures/asf/msmpeg4v3.asf msmpeg4v3
fixtures/asf/avi_div3.avi msmpeg4v3
fixtures/asf/wmv1_wma2.wmv wmv1
fixtures/asf/wmv2.wmv wmv2
fixtures/asf/avi_wmv2.avi wmv2
TABLE
for f in $WV/v2_odd.avi $WV/wmv1_noise.wmv $WV/wmv2_loop.mkv; do
    check "damaged copies of $(basename $f) decode or fail cleanly" python3 comparex.py fuzz $f $WV_FUZZ 9
done
check "AVI key frames come from the picture types (I-pictures 0 and 5 of 10)" wv_keys $WV/v2_odd.avi 2
check "six threads decoding a WMV8 file at once each get the straight decode" wv_threads $WV/wmv2_loop.mkv
if command -v leaks > /dev/null; then
    check "jumping around a WMV8 file leaks nothing" eval "leaks --atExit -- $A frames $WV/wmv2_loop.mkv --list 9,3,7,0,5,2 2> /dev/null | LC_ALL=C grep -aq ': 0 leaks for 0 total leaked bytes'"
fi

if have_ffmpeg; then
    echo "== Microsoft MPEG-4 v2 and v3, WMV7 and WMV8 encoded by ffmpeg at test time: sizes, bit rates, AVI, ASF and Matroska, every frame bit-exact"
    for f in $WV/*.avi $WV/*.mkv $WV/*.wmv fixtures/asf/msmpeg4v2.asf fixtures/asf/msmpeg4v3.asf fixtures/asf/avi_div3.avi fixtures/asf/wmv1_wma2.wmv fixtures/asf/wmv2.wmv fixtures/asf/avi_wmv2.avi; do
        check "decode $(basename $f) bit-exact against ffmpeg" $WK ordered $f
    done
    WV_SET="msmpeg4v2:176x144:100k:avi msmpeg4:640x480:2M:wmv wmv1:320x240:300k:mkv wmv2:720x576:4M:avi"
    [ -z "${QUICK:-}" ] && WV_SET="$WV_SET msmpeg4v2:352x288:600k:wmv msmpeg4v2:720x576:3M:mkv msmpeg4:176x144:64k:avi msmpeg4:352x288:q31:mkv msmpeg4:720x576:6M:avi wmv1:176x144:100k:avi wmv1:640x480:q2:wmv wmv1:720x576:2M:avi wmv2:176x144:80k:wmv wmv2:352x288:q2:mkv wmv2:640x480:1M:wmv"
    for item in $WV_SET; do
        IFS=: read codec size rate kind <<< "$item"
        out=$O/wv_${codec}_${size}_$rate.$kind
        case $rate in q*) quality="-q:v ${rate#q}" ;; *) quality="-b:v $rate" ;; esac
        filters="testsrc2=size=$size:rate=25"
        [ "$codec" = wmv2 ] && extra="-flags +loop" || extra=""
        $F -f lavfi -i "$filters" -frames:v 40 -g 15 -c:v $codec $quality $extra $out
        check "$codec $size $rate in $kind: every frame bit-exact against ffmpeg" $WK ordered $out
    done
    for f in $O/wv_msmpeg4_640x480_2M.wmv $O/wv_wmv2_720x576_4M.avi; do
        check "random frames of $(basename $f) against ffmpeg" python3 comparex.py seek $f 8 7
        check "frame times of $(basename $f) equal ffprobe's" python3 formats_check.py frametimes $f
    done
    $F -f lavfi -i "testsrc2=size=720x576:rate=25,noise=alls=8:allf=t" -frames:v 300 -c:v msmpeg4 -b:v 3M $O/wv_speed_v3.avi
    $F -f lavfi -i "testsrc2=size=720x576:rate=25,noise=alls=8:allf=t" -frames:v 300 -c:v wmv2 -b:v 3M $O/wv_speed_wmv2.avi
    for f in $O/wv_speed_v3.avi $O/wv_speed_wmv2.avi; do
        check "decode speed of $(basename $f) (720x576, 3 Mbit/s): at most 1.5 times ffmpeg's time on one thread" $WK speed $f 1.5
    done
    rm -f $O/wv_*
fi

echo "== Microsoft MPEG-4 v1 to v3, WMV7 and WMV8 sample files from WINMEDIA_SAMPLES (not committed)"
VS="${WINMEDIA_SAMPLES:-}"
if [ -z "$VS" ] || [ ! -d "$VS" ]; then
    skip "Microsoft MPEG-4 and WMV7/8 sample files (set WINMEDIA_SAMPLES to the sample folder)"
    return 0
fi
GOOFY="avi+msmpeg4+pcm_u8++Goofy_1948_The_Big_Wash-smallfile.avi"
FAULT="asf_msmpeg4v2_mp3_asf2divX_fault.asf"
while read f backend; do
    [ -f "$VS/$f" ] || continue
    check "$f decodes with FFamp's own $backend decoder" wv_backend "$VS/$f" $backend
    check "$f pictures decode to the checked checksum" wv_listed $(basename $f) "$VS/$f"
done <<TABLE
mpg4.avi msmpeg4v1
div3.avi msmpeg4v3
mp43.wmv msmpeg4v3
Park_auto_no_driver.wmv msmpeg4v3
$GOOFY msmpeg4v3
$FAULT msmpeg4v2
sample-wmv7.wmv wmv1
wmv1.wmv wmv1
sample-wmv8.1.wmv wmv2
fate/wmv8/wmv8_x8intra.wmv wmv2
TABLE
for f in mpg4.avi Park_auto_no_driver.wmv $GOOFY sample-wmv8.1.wmv fate/wmv8/wmv8_x8intra.wmv; do
    [ -f "$VS/$f" ] && check "random frames of $(basename $f) equal the straight decode" python3 comparex.py selfseek "$VS/$f" 12 5
done
[ -f "$VS/$GOOFY" ] && check "the DivX 3 AVI without an index takes its 6 key frames from the picture types" wv_keys "$VS/$GOOFY" 6
for f in sample-wmv8.1.wmv Park_auto_no_driver.wmv; do
    [ -f "$VS/$f" ] && check "runs of frames after jumps in $f equal the straight decode" python3 speedcheck.py steps "$VS/$f"
done
for f in mp43.wmv fate/wmv8/wmv8_x8intra.wmv; do
    [ -f "$VS/$f" ] && check "damaged copies of $(basename $f) decode or fail cleanly" python3 comparex.py fuzz "$VS/$f" $WV_FUZZ 9
done

have_ffmpeg || { skip "Microsoft MPEG-4 and WMV7/8 samples against ffmpeg"; return 0; }

echo "== Microsoft MPEG-4 v1 to v3, WMV7 and WMV8 samples against ffmpeg's decode, every frame"
for f in mpg4.avi div3.avi Park_auto_no_driver.wmv sample-wmv7.wmv wmv1.wmv sample-wmv8.1.wmv fate/wmv8/wmv8_x8intra.wmv; do
    [ -f "$VS/$f" ] && check "decode $(basename $f) bit-exact against ffmpeg" $WK ordered "$VS/$f"
done
[ -f "$VS/mp43.wmv" ] && check "decode mp43.wmv bit-exact against ffmpeg (ffmpeg also decodes the two cut-off media objects at the end)" $WK ordered "$VS/mp43.wmv" 2
[ -f "$VS/$GOOFY" ] && check "decode the DivX 3 AVI bit-exact against ffmpeg (told to ignore the index, it reads one cut-off chunk more)" $WK ordered "$VS/$GOOFY" 1 -fflags +ignidx
[ -f "$VS/$FAULT" ] && check "the damaged v2 ASF: ffmpeg's demuxer stops after 7 pictures, those 7 are identical and FFamp goes on" $WK prefix "$VS/$FAULT" 7
for f in Park_auto_no_driver.wmv sample-wmv8.1.wmv fate/wmv8/wmv8_x8intra.wmv; do
    [ -f "$VS/$f" ] && check "random frames of $(basename $f) against ffmpeg" python3 comparex.py seek "$VS/$f" 12 7
done
for f in mpg4.avi div3.avi sample-wmv7.wmv sample-wmv8.1.wmv; do
    [ -f "$VS/$f" ] && check "frame times of $(basename $f) equal ffprobe's" python3 formats_check.py frametimes "$VS/$f"
done
for f in Park_auto_no_driver.wmv sample-wmv8.1.wmv sample-wmv7.wmv; do
    [ -f "$VS/$f" ] && check "decode speed of $f: at most 1.5 times ffmpeg's time on one thread" $WK speed "$VS/$f" 1.5
done
