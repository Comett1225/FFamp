have_ffmpeg || return 0
echo "== videofix: video decoding findings of the 5.17 review, with ffmpeg as the judge"
VF=$O/videofix
mkdir -p $VF
VC="python3 videofix_check.py"
S="testsrc2=size=640x360:rate=25"
vf_make() { [ -s $VF/$1 ] || $F -f lavfi -i "$2" "${@:3}" $VF/$1 2> /dev/null || rm -f $VF/$1; }
vf_has() { "$FFMPEG" -hide_banner -encoders 2> /dev/null | grep -q " $1 "; }
vf_limit() { if command -v timeout > /dev/null; then timeout 300 "$@"; else "$@"; fi; }
vf_opens() { vf_limit $A videoinfo $VF/$1 > /dev/null; }
vf_backend() { vf_limit $A videoinfo $VF/$1 | grep -q "backend $2 "; }
vf_named() { $A probe --json $VF/$1 | grep -q "\"codec_name\":\"$2\""; }

for p in yuv422p10le yuv444p gray gray10le; do vf_make hevc_$p.mp4 "$S" -t 0.4 -c:v libx265 -x265-params log-level=error -pix_fmt $p -tag:v hvc1; done
for p in yuv422p10le yuv444p; do
    check "HEVC $p opens without hanging" vf_opens hevc_$p.mp4
    check "HEVC $p bit-exact against ffmpeg" $VC planes $VF/hevc_$p.mp4 $p
done
check "monochrome HEVC (gray): every frame, luma bit-exact" $VC luma $VF/hevc_gray.mp4 gray
if on_mac; then check "monochrome HEVC (gray10le): every frame, luma within 1 (VideoToolbox rounds a few samples)" $VC luma $VF/hevc_gray10le.mp4 gray10le 1
else check "monochrome HEVC (gray10le): every frame, luma bit-exact" $VC luma $VF/hevc_gray10le.mp4 gray10le; fi
if [ "$SYSTEM" = Windows ]; then
    for p in yuv422p10le yuv444p gray gray10le; do check "HEVC $p goes to libde265, not Media Foundation" vf_backend hevc_$p.mp4 libde265; done
fi

vf_make vp9_440.webm "$S" -t 0.4 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -pix_fmt yuv440p
if on_mac; then check "VP9 4:4:0: every frame, luma bit-exact (held as 4:4:4 on the Mac)" $VC luma $VF/vp9_440.webm yuv440p
else check "VP9 4:4:0 bit-exact against ffmpeg" $VC planes $VF/vp9_440.webm yuv440p; fi

ODD="testsrc2=size=1280x720:rate=25,format=rgb24,crop"
vf_make vp9_853x480.webm "$ODD=853:480:0:0" -t 0.4 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -pix_fmt yuv420p
vf_make vp9_640x361.webm "$ODD=640:361:0:0" -t 0.4 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -pix_fmt yuv420p
if vf_has libsvtav1; then vf_make av1_641x361.mp4 "$ODD=641:361:0:0" -t 0.4 -c:v libsvtav1 -pix_fmt yuv420p; fi
for f in vp9_853x480.webm vp9_640x361.webm av1_641x361.mp4; do
    [ -s $VF/$f ] || { skip "odd-sized $f (no encoder for it here)"; continue; }
    check "odd-sized $f opens without a crash" vf_opens $f
    check "odd-sized $f bit-exact against ffmpeg" $VC planes $VF/$f yuv420p
done

vf_make h264rgb.mp4 "$S" -t 0.4 -c:v libx264rgb -preset veryfast
vf_make vp9gbr.webm "$S" -t 0.4 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -pix_fmt gbrp
if vf_has libaom-av1; then vf_make av1gbr.mkv "$S" -t 0.4 -c:v libaom-av1 -cpu-used 8 -usage realtime -pix_fmt gbrp; fi
for f in h264rgb.mp4 vp9gbr.webm av1gbr.mkv; do
    [ -s $VF/$f ] || { skip "RGB-coded $f (no encoder for it here)"; continue; }
    if on_mac && [ $f = h264rgb.mp4 ]; then skip "RGB-coded $f (VideoToolbox does not decode H.264 4:4:4)"; continue; fi
    check "RGB-coded $f: BGRA equals ffmpeg's" $VC rgb $VF/$f
    check "RGB-coded $f: the RGBA frame equals ffmpeg's" $VC out $VF/$f rgba $VF/vf_out.rgba 0
    check "RGB-coded $f: the I420 frame is ffmpeg's BT.709 conversion within 1 on average" $VC out $VF/$f i420 $VF/vf_out.yuv 1
done

vf_make mjpa.mov "$S" -t 0.4 -c:v mjpeg -q:v 3 -pix_fmt yuvj422p -tag:v mjpa
vf_make mjpg.mov "$S" -t 0.4 -c:v mjpeg -q:v 3 -pix_fmt yuvj422p
check "QuickTime Motion JPEG A (mjpa) is read as JPEG" bash -c "$A probe $VF/mjpa.mov | grep -q 'video jpeg '"
check "mjpa decodes exactly as the same pictures tagged jpeg" $VC same $VF/mjpa.mov $VF/mjpg.mov

for c in msmpeg4v2 msmpeg4 wmv1 wmv2; do
    vf_make ms_$c.avi "$S" -t 1 -c:v $c -b:v 600k -g 25
    check "$c bit-exact against this machine's ffmpeg (x86-64 rounding included)" $VC planes $VF/ms_$c.avi yuv420p
done

vf_make m2_422.mkv "testsrc2=size=720x480:rate=30000/1001" -t 0.5 -c:v mpeg2video -q:v 3 -bf 2 -pix_fmt yuv422p
vf_make m2_422.mov "testsrc2=size=1280x720:rate=25" -t 0.5 -c:v mpeg2video -q:v 3 -bf 2 -pix_fmt yuv422p
for f in m2_422.mkv m2_422.mov; do check "MPEG-2 4:2:2 $f: every picture within 50 dB of ffmpeg" python3 comparex.py decode $VF/$f 50; done
if [ "$SYSTEM" = Windows ]; then check "MPEG-2 4:2:2 goes to libmpeg2, not Media Foundation" vf_backend m2_422.mkv libmpeg2; fi

vf_make dnxhd_1080.mov "testsrc2=size=1920x1080:rate=25" -t 0.2 -c:v dnxhd -b:v 36M -pix_fmt yuv422p
while read name pix alpha opts; do
    if [ $name = hap.mov ] && ! vf_has hap; then skip "HAP (this ffmpeg has no HAP encoder)"; break; fi
    vf_make $name "$S" -t 0.4 $opts
    [ -s $VF/$name ] || { skip "$name (no encoder for it here)"; continue; }
    check "$name decodes bit-exact against ffmpeg" $VC planes $VF/$name $pix $alpha
    [ $alpha = alpha ] && check "$name alpha equals ffmpeg's" $VC alpha $VF/$name
    check "damaged copies of $name never crash" python3 comparex.py fuzz $VF/$name 6 7
done <<TABLE
dnxhd_1080.mov yuv422p - -c:v dnxhd
dnxhr_hq.mov yuv422p - -c:v dnxhd -profile:v dnxhr_hq -pix_fmt yuv422p
dnxhr_hqx.mov yuv422p10le - -c:v dnxhd -profile:v dnxhr_hqx -pix_fmt yuv422p10le
qtrle.mov gbrap alpha -c:v qtrle -pix_fmt argb
qtrle24.mov gbrp - -c:v qtrle -pix_fmt rgb24
png.mov gbrap alpha -c:v png -pix_fmt rgba
png.avi gbrp - -c:v png -pix_fmt rgb24
v210.mov yuv422p10le - -c:v v210
raw.avi yuv420p - -c:v rawvideo -pix_fmt yuv420p
rawbgr.avi gbrp - -c:v rawvideo -pix_fmt bgr24
rawuyvy.mov yuv422p - -c:v rawvideo -pix_fmt uyvy422
ut.avi yuv420p - -c:v utvideo
ut_median.avi yuv422p - -c:v utvideo -pred median -pix_fmt yuv422p
ut_rgba.avi gbrap alpha -c:v utvideo -pix_fmt gbrap
ffv1.mkv yuv420p - -c:v ffv1 -g 5
ffv1_range.mkv yuv422p10le - -c:v ffv1 -coder 1 -slices 4 -pix_fmt yuv422p10le -g 5
ffv1_v1.avi yuv420p - -c:v ffv1 -level 1 -g 5
ffv1_rgb.mkv gbrp - -c:v ffv1 -pix_fmt bgr0 -g 5
ffv1_rgba.mkv gbrap alpha -c:v ffv1 -pix_fmt bgra -coder 1 -g 5
ffv1_v3.mov yuv420p - -c:v ffv1 -level 3 -slices 4 -g 5
hap.mov gbrp - -c:v hap
hap_alpha.mov gbrap alpha -c:v hap -format hap_alpha
hap_q.mov gbrp - -c:v hap -format hap_q
TABLE
if on_mac; then
    skip "12 and 16-bit FFV1 bit-exact (the Mac holds pictures in 8 or 10-bit buffers)"
else
    while read name pix alpha opts; do
        vf_make $name "$S" -t 0.4 $opts
        check "$name decodes bit-exact against ffmpeg" $VC planes $VF/$name $pix $alpha
    done <<TABLE
ffv1_444p12.mkv yuv444p12le - -c:v ffv1 -coder 1 -pix_fmt yuv444p12le -g 5
ffv1_420p16.mkv yuv420p16le - -c:v ffv1 -pix_fmt yuv420p16le -g 5
ffv1_rgb16.mkv gbrp16le - -c:v ffv1 -coder 1 -pix_fmt gbrp16le -g 5
ffv1_gbrap12.mkv gbrap12le alpha -c:v ffv1 -coder 1 -pix_fmt gbrap12le -g 5
TABLE
fi
while read name pix opts; do
    vf_make $name "$S" -t 0.4 $opts
    if on_mac; then check "$name: every frame, luma bit-exact (chroma held as 4:2:x on the Mac)" $VC luma $VF/$name $pix
    else check "$name decodes bit-exact against ffmpeg" $VC planes $VF/$name $pix -; fi
done <<TABLE
raw410.avi yuv410p -c:v rawvideo -pix_fmt yuv410p
raw411.avi yuv411p -c:v rawvideo -pix_fmt yuv411p
ffv1_411.mkv yuv411p -c:v ffv1 -pix_fmt yuv411p -g 5
TABLE
vf_colours() {
    $A compress $VF/$1 $VF/compressed_$1.mp4 --bitrate 8000000 > /dev/null 2>&1 || return 1
    python3 -c "
import subprocess, sys, numpy
def rgb(path): return numpy.frombuffer(subprocess.run([sys.argv[3], '-nostdin', '-v', 'error', '-i', path, '-frames:v', '4', '-vf', 'scale=in_color_matrix=bt709', '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-'], capture_output=True).stdout, numpy.uint8).astype(numpy.int32)
a, b = rgb(sys.argv[1]), rgb(sys.argv[2])
gap = numpy.abs(a - b).mean() if len(a) == len(b) > 0 else 255
print(f'{sys.argv[1]}: mean RGB difference {gap:.2f}')
sys.exit(0 if gap < 6 else 1)" $VF/$1 $VF/compressed_$1.mp4 "$FFMPEG"
}
for f in qtrle24.mov png.avi rawbgr.avi; do check "compress of $f keeps its colours (RGB pictures converted, not read as YCbCr)" vf_colours $f; done
vf_make dnxhd_1080i.mov "testsrc2=size=1920x1080:rate=25" -t 0.2 -c:v dnxhd -b:v 120M -pix_fmt yuv422p -flags +ildct
check "interlaced DNxHD decodes bit-exact against ffmpeg" $VC planes $VF/dnxhd_1080i.mov yuv422p
check "DNxHD whose second field claims more rows than its first is refused, not written past the picture" $VC taller $VF/dnxhd_1080i.mov $VF/dnxhd_taller.mov
vf_make ffv1_gray.avi "$S" -t 0.4 -c:v ffv1 -pix_fmt gray
check "gray FFV1 with no range label is full range: BGRA equals ffmpeg's" $VC rgb $VF/ffv1_gray.avi
vf_make prores4444.mov "testsrc2=size=320x240:rate=25" -t 0.4 -c:v prores_ks -profile:v 4 -pix_fmt yuv444p10le
check "ProRes 4444 whose frame header gives a colour but matrix 0 (unknown) is not taken as RGB" $VC prores-matrix $VF/prores4444.mov $VF/prores4444_m0.mov
vf_make yuv9.avi "$S" -t 0.4 -c:v rawvideo -pix_fmt yuv410p
vf_make ffv1_410.mkv "$S" -t 0.4 -c:v ffv1 -pix_fmt yuv410p
for f in yuv9.avi ffv1_410.mkv; do check "4:1:0 $f through SoftVideo: chroma rows where ffmpeg puts them (mean within 3)" $VC soft-chroma $VF/$f 3; done
vf_make h264_10_slices.mp4 "testsrc2=size=320x240:rate=25" -t 0.2 -c:v libx264 -preset veryfast -pix_fmt yuv420p10le -x264-params slices=4:keyint=1
if [ -s $VF/h264_10_slices.mp4 ]; then check "10-bit H.264 with a damaged slice: concealment leaves its macroblocks decoded before the damage alone" $VC damaged-slice $VF/h264_10_slices.mp4 $VF/h264_10_damaged.mp4
else skip "10-bit H.264 concealment (no libx264 here)"; fi
for f in qtrle.mov ffv1.mkv ffv1_range.mkv; do check "random frames of $f equal the straight decode" python3 comparex.py selfseek $VF/$f 8 3; done
for pair in dnxhr_hq.mov:dnxhd qtrle.mov:qtrle png.mov:png v210.mov:v210 raw.avi:rawvideo ut.avi:utvideo ffv1.mkv:ffv1; do
    check "probe names ${pair%%:*} as ffprobe does (${pair##*:})" vf_named ${pair%%:*} ${pair##*:}
done
