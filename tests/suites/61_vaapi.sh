echo "== VA-API on Linux: H.264, HEVC, VP9 and AV1 on the GPU when libva and a driver load (Mesa's d3d12 driver under WSL), every frame bit-exact against ffmpeg"
if [ "$SYSTEM" != Linux ]; then
    skip "VA-API (Linux only)"
    return 0
fi
have_ffmpeg || { skip "VA-API clips encoded by ffmpeg at test time"; return 0; }
va_backend() { [ "$(env ${3:+FFAMP_NO_VAAPI=1} $A videoinfo "$1" | sed -n 's/.*backend \([^ ]*\).*/\1/p')" = "$2" ]; }
va_not() { ! va_backend "$1" vaapi; }
VA_SRC="testsrc2=size=640x360:rate=30,noise=alls=10:allf=t"
SVT_LOG=1 $F -f lavfi -i "$VA_SRC" -frames:v 60 -c:v libx264 -preset fast -bf 3 -g 30 $O/va_h264.mp4
if ! va_backend $O/va_h264.mp4 vaapi; then
    skip "VA-API decoding (no VA-API driver loads on this machine; the software decoders are checked in the other suites)"
    rm -f $O/va_*
    return 0
fi
VA_D3D12=
[ -e /dev/dxg ] && VA_D3D12=1
VA_SET="
h264_weighted.mp4|fade=in:0:30|-c:v libx264 -preset fast -bf 2 -weightp 2 -weightb 1 -g 60
h264_mbaff.mp4||-c:v libx264 -preset fast -flags +ildct+ilme -x264-params tff=1 -g 30
h264_slices.mp4||-c:v libx264 -preset fast -slices 4 -bf 0 -g 15
h264_odd.mp4|crop=630:350|-c:v libx264 -preset fast -bf 2 -g 20
hevc_8.mp4||-c:v libx265 -preset fast -x265-params log-level=error:keyint=30:bframes=3 -tag:v hvc1
hevc_10.mp4||-pix_fmt yuv420p10le -c:v libx265 -preset fast -x265-params log-level=error:keyint=30 -tag:v hvc1
vp9_8.webm||-c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 1M -g 30
vp9_10.webm|crop=640:352|-pix_fmt yuv420p10le -profile:v 2 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 1M -g 30
av1_8.mp4||-c:v libsvtav1 -preset 10 -g 30
av1_10.mp4|crop=640:352|-pix_fmt yuv420p10le -c:v libsvtav1 -preset 10 -g 30
"
check "va_h264.mp4 decodes on the GPU" va_backend $O/va_h264.mp4 vaapi
check "H.264 with B-frames on the GPU: every frame bit-exact against ffmpeg" python3 comparex.py decode $O/va_h264.mp4
check "random frames of va_h264.mp4 on the GPU equal the straight decode" python3 comparex.py selfseek $O/va_h264.mp4 12 5
check "FFAMP_NO_VAAPI keeps H.264 on FFamp's own decoder" va_backend $O/va_h264.mp4 h264 1
while IFS="|" read name filter options; do
    [ -n "$name" ] || continue
    out=$O/va_$name
    SVT_LOG=1 $F -f lavfi -i "$VA_SRC${filter:+,$filter}" -frames:v 60 $options $out
    check "$name decodes on the GPU" va_backend $out vaapi
    check "$name on the GPU: every frame bit-exact against ffmpeg" python3 comparex.py decode $out
    check "random frames of $name on the GPU equal the straight decode" python3 comparex.py selfseek $out 12 5
done <<< "$VA_SET"
SVT_LOG=1 $F -f lavfi -i "$VA_SRC" -frames:v 60 -c:v libx264 -preset fast -bf 0 -x264-params cqm=jvt -g 30 $O/va_h264_cqm.mp4
SVT_LOG=1 $F -f lavfi -i "$VA_SRC" -frames:v 30 -pix_fmt yuv420p10le -profile:v 2 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 1M $O/va_vp9_10_360.webm
SVT_LOG=1 $F -f lavfi -i "$VA_SRC" -frames:v 30 -pix_fmt yuv420p10le -c:v libsvtav1 -preset 10 $O/va_av1_10_360.mp4
if [ -n "$VA_D3D12" ]; then
    check "H.264 with scaling matrices goes to FFamp's own decoder under the d3d12 driver (it ignores them)" va_backend $O/va_h264_cqm.mp4 h264
    check "10-bit VP9 360 lines high stays on libvpx under the d3d12 driver (wrong pictures when height % 16 is 2 to 9)" va_not $O/va_vp9_10_360.webm
    check "10-bit AV1 360 lines high stays on dav1d under the d3d12 driver (the same heights)" va_not $O/va_av1_10_360.mp4
else
    check "H.264 with scaling matrices decodes on the GPU" va_backend $O/va_h264_cqm.mp4 vaapi
fi
for f in va_h264_cqm.mp4 va_vp9_10_360.webm va_av1_10_360.mp4; do check "$f: every frame bit-exact against ffmpeg" python3 comparex.py decode $O/$f; done
rm -f $O/va_*
