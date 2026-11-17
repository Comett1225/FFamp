echo "== Android route: FFamp's own decoders where the phone has no MediaCodec decoder; sound as Android opens it (FFAMP_NO_SYSTEM_AUDIO here), pictures through SoftVideo as I420 in presentation order"
have_ffmpeg || { skip "the Android route against ffmpeg"; return 0; }
for f in avi_asp_ac3.avi ac3_51.ac3 eac3_51.mp4 caf_flac.caf caf_alac.caf caf_aac.caf; do
    check "$f sound through FFamp's own decoder within 135 dB of ffmpeg" env FFAMP_NO_SYSTEM_AUDIO=1 python3 audio.py against fixtures/formats/$f 135 0 0 peer
done
AN=$O/android
rm -rf $AN
mkdir -p $AN
AN_SRC="testsrc2=size=352x288:rate=25"
$F -f lavfi -i $AN_SRC -t 2 -c:v msmpeg4 -q:v 4 $AN/divx3.avi
$F -f lavfi -i $AN_SRC -t 2 -c:v cinepak $AN/cinepak.avi
$F -f lavfi -i $AN_SRC -t 2 -c:v utvideo -pix_fmt yuv420p $AN/utvideo.avi
$F -f lavfi -i $AN_SRC -t 2 -c:v ffv1 -pix_fmt yuv420p $AN/ffv1.mkv
$F -f lavfi -i $AN_SRC -t 2 -c:v qtrle $AN/qtrle.mov
$F -f lavfi -i $AN_SRC -t 2 -c:v png $AN/png.mov
$F -f lavfi -i $AN_SRC -t 2 -c:v prores_ks -profile:v 2 $AN/prores.mov
$F -f lavfi -i testsrc2=size=1280x720:rate=25 -t 1 -c:v dnxhd -profile:v dnxhr_lb -pix_fmt yuv422p $AN/dnxhr.mov
$F -f lavfi -i $AN_SRC -t 2 -c:v libx264 -preset veryfast -bf 3 -pix_fmt yuv420p10le $AN/h264_10bit.mkv
$F -f lavfi -i $AN_SRC -t 2 -c:v libx264 -preset veryfast -bf 3 -pix_fmt yuv422p $AN/h264_422.mkv
on_mac && $F -f lavfi -i testsrc2=size=320x240:rate=25 -t 3 -pix_fmt yuv420p -c:v $FF_H264 -bf 2 $AN/h264_no_vui.mp4
sv_exact() { [ "$($A softvideo "$1" 2> /dev/null | md5)" = "$($F -i "$1" -map 0:v:0 -fps_mode passthrough -pix_fmt yuv420p -f rawvideo - | md5)" ]; }
sv_all() { [ "$($A softvideo "$1" 2>&1 > /dev/null | sed -n 's/.* pictures \([0-9]*\),.*/\1/p')" = "$($FFPROBE -v error -select_streams v:0 -count_frames -show_entries stream=nb_read_frames -of csv=p=0 "$1")" ]; }
for f in fixtures/video/h264_270p.mp4 fixtures/video/h264_paff_64p.mp4 fixtures/video/h264_mbaff_272p.mp4 $(on_mac && echo $AN/h264_no_vui.mp4) $AN/divx3.avi $AN/utvideo.avi $AN/ffv1.mkv; do
    check "$(basename $f) through SoftVideo equals ffmpeg's yuv420p pictures" sv_exact $f
done
for f in fixtures/formats/h263.3gp fixtures/formats/dv_pal.mov fixtures/formats/dv_ntsc.mov $AN/cinepak.avi $AN/qtrle.mov $AN/png.mov $AN/prores.mov $AN/dnxhr.mov $AN/h264_10bit.mkv $AN/h264_422.mkv; do
    check "$(basename $f) through SoftVideo gives every picture in order" sv_all $f
done
sv_rgb() { $A softvideo "$1" 2>&1 > /dev/null | grep -q "pictures \([1-9][0-9]*\), $2 of them from RGB"; }
for f in $AN/qtrle.mov $AN/png.mov; do
    check "$(basename $f) through SoftVideo marks every picture as made from RGB" sv_rgb $f '\1'
done
for f in $AN/utvideo.avi $AN/prores.mov fixtures/video/h264_270p.mp4; do
    check "$(basename $f) through SoftVideo marks no picture as made from RGB" sv_rgb $f 0
done
rm -rf $AN
