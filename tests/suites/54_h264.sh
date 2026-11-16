echo "== H.264: FFamp's own decoder (FFmpeg's, ported to src/h264); the Mac decodes H.264 with VideoToolbox and runs it here only through ffamp h264"
HV=fixtures/video
H4_FUZZ=12
[ -n "${QUICK:-}" ] && H4_FUZZ=4
h4_backend() { $A videoinfo "$1" --software | grep -q "backend h264 "; }
h4_fuzz() {
    kept=(fixtures/fuzz/h264/*)
    env TMPDIR=$O ../build/fuzz_h264 "${kept[@]}" && env TMPDIR=$O ../build/fuzz_h264 -seed=7 -max_total_time=${FUZZ_BURST:-3} -rss_limit_mb=2048 -artifact_prefix=$O/ $O/h4_corpus fixtures/fuzz/h264
}
if [ -x ../build/fuzz_h264 ]; then
    mkdir -p $O/h4_corpus
    check "h264 fuzz target: kept inputs replay and ${FUZZ_BURST:-3} s of mutated streams find nothing" h4_fuzz 2> /dev/null
    rm -rf $O/h4_corpus
else
    skip "h264 fuzz target (the fuzz targets are not built on this system)"
fi
if ! on_mac; then
    check "H.264 fixtures decode with FFamp's own decoder when the system decoder is not used" h4_backend $HV/h264_270p.mp4
    for f in $HV/h264_270p.mp4 $HV/h264_mbaff_272p.mp4 $HV/h264_paff_64p.mp4 fixtures/formats/avi_h264_mp3.avi; do
        check "random frames of $(basename $f) equal the straight decode (own decoder)" python3 comparex.py selfseek $f 12 5 --software
        check "damaged copies of $(basename $f) decode or fail cleanly (own decoder)" python3 comparex.py fuzz $f $H4_FUZZ 9 --software
    done
fi

have_ffmpeg || { skip "H.264 encoded by ffmpeg at test time"; return 0; }

for f in $HV/h264_270p.mp4 $HV/h264_mbaff_272p.mp4 $HV/h264_paff_64p.mp4 fixtures/formats/avi_h264_mp3.avi; do
    check "$(basename $f): every frame bit-exact against ffmpeg (one thread, frame threads, slice threads)" python3 h264_check.py exact $f
done

echo "== H.264 encoded by x264 at test time: every profile, interlaced, 9 to 10 bits, 4:2:2, 4:4:4, lossless, RGB, monochrome, B-pyramids, weighted prediction, slices, cropping, intra refresh; every frame bit-exact"
H4_SRC="testsrc2=size=SIZE:rate=25,noise=alls=12:allf=t"
H4_SET="
base|320x240|30|yuv420p|libx264|-profile:v baseline -preset fast
base_slices|352x288|24|yuv420p|libx264|-profile:v baseline -slices 4
main_b3|320x240|30|yuv420p|libx264|-profile:v main -bf 3 -b_strategy 2
high_pyramid|320x240|40|yuv420p|libx264|-x264opts bframes=4:b-pyramid=normal
high_weighted|320x240|30|yuv420p|libx264|-x264opts weightp=2:weightb=1:bframes=3:b-pyramid=strict
high_temporal|352x288|24|yuv420p|libx264|-x264opts 8x8dct=1:ref=6:bframes=2:direct=temporal
high_cavlc|320x240|24|yuv420p|libx264|-x264opts cabac=0:8x8dct=1:bframes=2
high_odd|302x182|24|yuv420p|libx264|-bf 2
mbaff|352x288|24|yuv420p|libx264|-flags +ildct -x264opts tff=1:bframes=2
mbaff_bff_cavlc|352x288|24|yuv420p|libx264|-flags +ildct -x264opts bff=1:cabac=0:bframes=2:weightp=2
hi10|320x240|24|yuv420p10le|libx264|-profile:v high10 -bf 3
hi422_10|320x240|24|yuv422p10le|libx264|-profile:v high422 -bf 2
hi444|320x240|24|yuv444p|libx264|-profile:v high444 -bf 2
lossless|320x240|16|yuv444p|libx264|-qp 0 -preset fast
rgb|320x240|16|bgr24|libx264rgb|-bf 2
gray|320x240|20|gray|libx264|-bf 2
"
[ -z "${QUICK:-}" ] && H4_SET="$H4_SET
high_slices|640x360|16|yuv420p|libx264|-slices 6 -bf 2
high_open_gop|320x240|60|yuv420p|libx264|-g 20 -x264opts open-gop=1:bframes=3
high_constrained|320x240|24|yuv420p|libx264|-x264opts constrained-intra=1:bframes=2
high_refresh|320x240|60|yuv420p|libx264|-x264opts intra-refresh=1:keyint=20:bframes=0
high_crop_left|320x240|20|yuv420p|libx264|-x264-params crop-rect=40,2,6,0
high_fake_interlaced|320x240|24|yuv420p|libx264|-x264opts fake-interlaced=1:pic-struct=1
high_crop|320x240|20|yuv420p|libx264|-vf crop=318:236:0:0 -bf 2
high_ref16|320x240|30|yuv420p|libx264|-x264opts ref=16:bframes=3:b-adapt=2
high_slice_size|352x288|20|yuv420p|libx264|-x264opts slice-max-size=600:bframes=2
mbaff_cavlc|352x288|24|yuv420p|libx264|-flags +ildct -x264opts tff=1:cabac=0:bframes=2
hi10_mbaff|352x288|16|yuv420p10le|libx264|-flags +ildct -x264opts tff=1:bframes=2
hi422|320x240|24|yuv422p|libx264|-profile:v high422 -bf 2
hi422_mbaff|352x288|16|yuv422p|libx264|-flags +ildct -x264opts tff=1
hi444_10|320x240|24|yuv444p10le|libx264|-profile:v high444 -bf 2
hi444_cavlc|320x240|16|yuv444p|libx264|-profile:v high444 -x264opts cabac=0
lossless_cavlc|320x240|16|yuv444p|libx264|-qp 0 -x264opts cabac=0
lossless420|320x240|16|yuv420p|libx264|-qp 0 -preset fast
rgb_lossless|320x240|16|bgr24|libx264rgb|-qp 0
intra|320x240|16|yuv420p|libx264|-x264opts keyint=1
hd|1280x720|10|yuv420p|libx264|-bf 3 -preset fast
"
while IFS='|' read name size frames pixfmt codec options; do
    [ -n "$name" ] || continue
    out=$O/h4_$name.mkv
    $F -f lavfi -i "${H4_SRC/SIZE/$size}" -frames:v $frames -pix_fmt $pixfmt -c:v $codec $options $out
    check "$codec $pixfmt $size $options: every frame bit-exact against ffmpeg (one thread, frame threads, slice threads)" python3 h264_check.py exact $out
done <<< "$H4_SET"
if ! on_mac; then
    for name in high_pyramid mbaff hi10 high_open_gop high_refresh; do
        [ -f $O/h4_$name.mkv ] || continue
        check "random frames of h4_$name.mkv equal ffmpeg's (own decoder)" python3 comparex.py seek $O/h4_$name.mkv 50 3 --software
    done
fi

check "H.274 film grain SEI written into an x264 stream (on every picture, and once with a repetition period): every frame bit-exact against ffmpeg, which applies the grain" python3 h264_check.py grain $O

echo "== H.264 damaged copies: decode with one thread and with frame threads, bit-exact against ffmpeg on one thread wherever FFmpeg's own output is defined"
$F -f lavfi -i "testsrc2=size=640x360:rate=25,noise=alls=10:allf=t" -frames:v 60 -c:v libx264 -bf 3 -g 30 -slices 4 $O/h4_damage_slices.mp4
$F -f lavfi -i "testsrc2=size=352x288:rate=25,noise=alls=10:allf=t" -frames:v 40 -c:v libx264 -flags +ildct -x264opts tff=1:bframes=2 $O/h4_damage_mbaff.mp4
$F -f lavfi -i "testsrc2=size=320x240:rate=25" -frames:v 50 -c:v libx264 -profile:v baseline -slices 3 $O/h4_damage_base.mp4
for f in slices mbaff base; do
    check "damaged copies of h4_damage_$f.mp4 against ffmpeg" python3 h264_check.py damaged $O/h4_damage_$f.mp4 $H4_FUZZ 3 $O
done

if [ -n "${H264_CONFORMANCE:-}" ] && [ -d "$H264_CONFORMANCE" ]; then
    check "H.264 conformance bitstreams in $H264_CONFORMANCE: every frame bit-exact against ffmpeg" python3 h264_check.py conformance "$H264_CONFORMANCE" $O
else
    skip "H.264 conformance bitstreams (set H264_CONFORMANCE to a folder of them)"
fi
rm -f $O/h4_*
