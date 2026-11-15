echo "== H.263: FFamp's own decoder on Windows and Linux (the Mac decodes H.263 with VideoToolbox, checked in the formats suite)"
if on_mac; then
    skip "FFamp's own H.263 decoder (not used on the Mac)"
    return 0
fi
HX=fixtures/formats
HX_FUZZ=12
[ -n "${QUICK:-}" ] && HX_FUZZ=4
hx_backend() { $A videoinfo "$1" | grep -q "backend h263 "; }
hx_keys() { [ "$($A packets "$1" | grep -c ' K ')" = "$2" ]; }
check "h263.3gp decodes with FFamp's own h263 decoder" hx_backend $HX/h263.3gp
check "random frames of h263.3gp equal the straight decode" python3 comparex.py selfseek $HX/h263.3gp 12 5
check "damaged copies of h263.3gp decode or fail cleanly" python3 comparex.py fuzz $HX/h263.3gp $HX_FUZZ 9

have_ffmpeg || { skip "H.263 encoded by ffmpeg at test time"; return 0; }

echo "== H.263 and H.263+ encoded by ffmpeg at test time: picture formats, custom sizes, GOBs, slices, 4MV, OBMC, AIC, deblocking, UMV, alternative inter VLC; 3GP, MOV, AVI, Matroska; every frame bit-exact"
HX_DARK="lutyuv=y=val*1.4-60:u=val*1.8-100:v=val*1.8-100"
HX_SET="
v1_sqcif:h263:128x96:25:3gp:-q:v 2 -g 5
v1_qcif:h263:176x144:25:avi:-q:v 8 -g 30
v1_cif:h263:352x288:25:mkv:-q:v 4 -g 12
v1_gob:h263:352x288:25:3gp:-q:v 5 -g 20 -ps 300
v1_obmc:h263:352x288:25:avi:-q:v 4 -g 15 -flags +mv4 -obmc 1
p_all:h263p:352x288:25:mov:-q:v 3 -g 12 -flags +aic+loop+mv4 -umv 1 -aiv 1 -obmc 1
p_slices:h263p:320x240:30000/1001:mov:-q:v 4 -g 15 -flags +aic -structured_slices 1 -ps 400
p_odd:h263p:100x76:25:avi:-q:v 3 -g 9 -flags +mv4+loop -umv 1 -obmc 1
p_aic:h263p:176x144:30000/1001:3gp:-q:v 6 -g 25 -flags +aic
"
[ -z "${QUICK:-}" ] && HX_SET="$HX_SET
v1_q31:h263:352x288:25:mov:-q:v 31 -g 250
v1_4cif:h263:704x576:25:3gp:-q:v 3 -g 10
v1_4cif_gob:h263:704x576:30000/1001:mkv:-b:v 1M -g 15 -ps 500
v1_16cif:h263:1408x1152:25:mov:-q:v 6 -g 8
v1_mv4:h263:176x144:25:mkv:-q:v 4 -g 15 -flags +mv4
p_umv:h263p:640x480:25:mkv:-q:v 5 -g 20 -flags +loop+mv4 -umv 1
p_aiv:h263p:352x288:25:avi:-q:v 2 -g 10 -aiv 1
p_obmc:h263p:352x288:25:mov:-q:v 4 -g 10 -flags +mv4 -obmc 1
p_720:h263p:1280x720:25:mkv:-q:v 5 -g 12 -flags +aic+loop -umv 1
p_plain:h263p:640x480:25:3gp:-b:v 800k -g 30
p_wide:h263p:1024x576:25:mkv:-q:v 4 -g 12 -flags +aic+loop+mv4 -umv 1 -aiv 1 -obmc 1 -structured_slices 1 -ps 1000
"
while IFS=: read name codec size rate kind options; do
    [ -n "$name" ] || continue
    out=$O/hx_$name.$kind
    if [ "$codec" = h263p ] && [ "$kind" = 3gp ]; then
        $F -f lavfi -i "testsrc2=size=$size:rate=$rate,$HX_DARK" -frames:v 40 -c:v h263p $options -f rawvideo $O/hx_$name.raw
        $F -f h263 -framerate $rate -i $O/hx_$name.raw -c copy $out
        rm -f $O/hx_$name.raw
        check "$codec $size $options in $kind (through ffmpeg's raw H.263 reader, every picture a key frame): every frame bit-exact against ffmpeg" python3 comparex.py decode $out
        continue
    fi
    $F -f lavfi -i "testsrc2=size=$size:rate=$rate,$HX_DARK" -frames:v 40 -c:v $codec $options $out
    check "$codec $size $options in $kind: every frame bit-exact against ffmpeg" python3 comparex.py decode $out
    check "random frames of hx_$name.$kind equal the straight decode" python3 comparex.py selfseek $out 10 5
done <<< "$HX_SET"
check "the H.263 encodes decode with FFamp's own h263 decoder" hx_backend $O/hx_p_all.mov
check "AVI key frames come from the picture types (I-pictures 0 and 30 of 40)" hx_keys $O/hx_v1_qcif.avi 2
for f in $O/hx_v1_qcif.avi $O/hx_p_aic.3gp; do check "frame times of $(basename $f) equal ffprobe's" python3 formats_check.py frametimes $f; done
for f in $O/hx_v1_gob.3gp $O/hx_p_all.mov $O/hx_p_slices.mov $O/hx_p_odd.avi; do
    check "damaged copies of $(basename $f) decode or fail cleanly" python3 comparex.py fuzz $f $HX_FUZZ 9
done
rm -f $O/hx_*
