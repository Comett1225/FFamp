echo "== videox: new decoders on committed fixtures, no FFmpeg needed"
VX=fixtures/video
vx_sum() { [ "$($A decode "$VX/$1" ${2:-} | md5)" = "$(grep " $1\$" $VX/decodedx.md5 | cut -d' ' -f1)" ]; }
vx_backend() { $A videoinfo "$VX/$1" ${3:-} | grep -q "backend $2 "; }
for f in hevc10_270p.mp4 hdr_pq_270p.mp4 hdr_hlg_270p.mp4 h264_mbaff_272p.mp4 h264_paff_64p.mp4 vp9_270p.mp4 vp9_10bit_270p.mp4 vp9_270p.ivf vp8_270p.ivf av1_270p.mp4 av1_10bit_270p.mp4 av1_270p.ivf; do
    check "decode $f matches the checked checksum" vx_sum $f
    check "seek $f equals the straight decode" python3 comparex.py selfseek $VX/$f 24 5
done
for f in vp9_270p.mp4 vp9_10bit_270p.mp4 vp9_270p.ivf av1_270p.mp4 av1_10bit_270p.mp4 av1_270p.ivf; do
    case $f in vp9*) library=libvpx ;; *) library=dav1d ;; esac
    if on_mac; then check "$f opens with the system decoder" vx_backend $f videotoolbox; fi
    check "$f opens with $library when asked" vx_backend $f $library --software
    check "software decode $f matches the checked checksum" vx_sum $f --software
    check "software seek $f equals the straight decode" python3 comparex.py selfseek $VX/$f 24 5 --software
done
check "10-bit HEVC comes out as 10-bit 4:2:0" [ "$($A decode $VX/hevc10_270p.mp4 | wc -c | tr -d ' ')" = $((480 * 270 * 3 * 48)) ]
check "field-coded H.264 gives 15 woven frames from 30 field samples" [ "$($A decode $VX/h264_paff_64p.mp4 | wc -c | tr -d ' ')" = $((96 * 64 * 3 / 2 * 15)) ]
check "ProRes decodes to the checked plane levels" python3 comparex.py means $VX/prores_180p.mov $VX/prores_180p.means
for f in hdr_pq_270p.mp4 hdr_hlg_270p.mp4 hevc10_270p.mp4 h264_270p.mp4 prores_180p.mov vp8_270p.ivf; do
    check "BGRA preview of $f follows the reference formula" python3 comparex.py selfbgra $VX/$f 0.999
done
for f in vp9_270p.mp4 av1_270p.ivf h264_paff_64p.mp4 prores_180p.mov hevc10_270p.mp4; do check "damaged $f never crashes" python3 comparex.py fuzz $VX/$f 30 1; done
for f in vp9_270p.ivf av1_270p.mp4 vp8_270p.ivf; do check "damaged $f never crashes the software decoder" python3 comparex.py fuzz $VX/$f 30 2 --software; done
for f in vp9_270p.mp4 av1_10bit_270p.mp4 hevc10_270p.mp4; do check "bench $f runs" $A bench $VX/$f > /dev/null; done

echo "== videox: AV1 and VP9 encoders, BGRA input and surfaces, no FFmpeg needed"
check "BGRA to I420 is within one code value of the system converter" $A pixels --size 640x360
$A decode $VX/h264_270p.mp4 > $O/vx_src.yuv
$A decode $VX/h264_270p.mp4 --bgra > $O/vx_src.bgra
$A pixels --size 480x270 --video $O/vx_src.bgra --out $O/vx_ref.yuv --no-system
for c in av1 vp9; do
    $A encode $O/vx_$c.ivf --video $O/vx_src.yuv --size 480x270 --fps 30 --codec $c --bitrate 800000 --gop 15 2> /dev/null
    check "$c from I420: both own decoders agree, PSNR over 36 dB" python3 comparex.py quality $O/vx_$c.ivf $O/vx_src.yuv 480 270 36
    check "$c has a key frame every 15 frames" [ "$($A probe $O/vx_$c.ivf | sed 's/.*keyframes \([0-9]*\).*/\1/')" = 4 ]
    check "$c seeks to exact frames" python3 comparex.py selfseek $O/vx_$c.ivf 20 3
    $A encode $O/vx_bgra_$c.ivf --video $O/vx_src.bgra --size 480x270 --fps 30 --codec $c --format bgra --bitrate 1500000 2> /dev/null
    check "$c from BGRA: PSNR over 38 dB" python3 comparex.py quality $O/vx_bgra_$c.ivf $O/vx_ref.yuv 480 270 38
    $A transcode $VX/hevc10_270p.mp4 $O/vx_t10_$c.ivf --codec $c --bitrate 1000000 2> /dev/null
    check "10-bit HEVC transcodes to $c" [ "$($A decode $O/vx_t10_$c.ivf | wc -c | tr -d ' ')" = $((480 * 270 * 3 / 2 * 48)) ]
    $A transcode $VX/prores_180p.mov $O/vx_tp_$c.ivf --codec $c 2> /dev/null
    check "4:2:2 ProRes transcodes to $c" [ "$($A decode $O/vx_tp_$c.ivf | wc -c | tr -d ' ')" = $((320 * 180 * 3 / 2 * 12)) ]
done
for c in h264 hevc av1 vp9; do
    case $c in h264 | hevc) e=mp4 ;; *) e=ivf ;; esac
    $A encode-surface $O/vx_surface_$c.$e --video $O/vx_src.bgra --size 480x270 --codec $c --bitrate 1500000 2> /dev/null
    if on_mac; then check "$c straight from BGRA surfaces: PSNR over 38 dB" python3 comparex.py quality $O/vx_surface_$c.$e $O/vx_ref.yuv 480 270 38; else skip "$c straight from BGRA surfaces (IOSurface input exists only on the Mac)"; fi
done
check "surface H.264 opens in AVFoundation" ../build/avcheck $O/vx_surface_h264.mp4
check "surface HEVC opens in AVFoundation" ../build/avcheck $O/vx_surface_hevc.mp4
$A transcode $VX/h264_paff_64p.mp4 $O/vx_paff.mp4 --bitrate 400000 2> /dev/null
check "field-coded H.264 transcodes to 15 progressive frames" [ "$($A probe $O/vx_paff.mp4 | sed 's/.*rotation [0-9]* frames \([0-9]*\) .*/\1/')" = 15 ]
rm -f $O/vx_src.bgra

have_ffmpeg || return 0
echo "== videox: every new decode case against ffmpeg"
for f in hevc10_270p.mp4 hdr_pq_270p.mp4 hdr_hlg_270p.mp4 h264_mbaff_272p.mp4 h264_paff_64p.mp4 vp9_270p.mp4 vp9_10bit_270p.mp4 vp9_270p.ivf vp8_270p.ivf av1_270p.mp4 av1_10bit_270p.mp4 av1_270p.ivf; do
    check "decode $f" python3 comparex.py decode $VX/$f
done
for f in vp9_270p.mp4 vp9_10bit_270p.mp4 av1_270p.mp4 av1_10bit_270p.mp4; do check "software decode $f" python3 comparex.py decode $VX/$f --software; done
check "ProRes within 60 dB of ffmpeg" python3 comparex.py decode $VX/prores_180p.mov 60
check "HDR PQ preview against the app's formula on ffmpeg pixels" python3 comparex.py hdr $VX/hdr_pq_270p.mp4 0.5 0.98
check "HDR HLG preview against the app's formula on ffmpeg pixels" python3 comparex.py hdr $VX/hdr_hlg_270p.mp4 0.5 0.98
check "SDR preview against the app's scaler settings" python3 comparex.py sdr $VX/h264_270p.mp4 0.1 0.999
for c in av1 vp9; do check "$c output decodes the same in ffmpeg" python3 comparex.py quality $O/vx_$c.ivf $O/vx_src.yuv 480 270 36 ffmpeg; done

echo "== videox: longer 720p clips, 40 random seeks each against ffmpeg"
LONG="testsrc2=size=1280x720:rate=30"
[ -f $O/vx_hevc10_720.mp4 ] || $F -f lavfi -i $LONG -t 4 -c:v libx265 -preset fast -x265-params log-level=error -g 60 -pix_fmt yuv420p10le -tag:v hvc1 $O/vx_hevc10_720.mp4
[ -f $O/vx_vp9_720.mp4 ] || $F -f lavfi -i $LONG -t 4 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -row-mt 1 -b:v 3M -g 60 -pix_fmt yuv420p $O/vx_vp9_720.mp4
[ -f $O/vx_av1_720.mp4 ] || SVT_LOG=1 $F -f lavfi -i $LONG -t 4 -c:v libsvtav1 -preset 10 -b:v 3M -g 60 -pix_fmt yuv420p $O/vx_av1_720.mp4
[ -f $O/vx_av1_10bit_720.mp4 ] || SVT_LOG=1 $F -f lavfi -i $LONG -t 4 -c:v libsvtav1 -preset 10 -b:v 3M -g 60 -pix_fmt yuv420p10le $O/vx_av1_10bit_720.mp4
[ -f $O/vx_mbaff_720.mp4 ] || $F -f lavfi -i mandelbrot=size=1280x720:rate=30 -t 4 -c:v libx264 -preset veryfast -flags +ildct+ilme -g 60 -b:v 4M -pix_fmt yuv420p $O/vx_mbaff_720.mp4
[ -f $O/vx_mbaff_bars_720.mp4 ] || $F -f lavfi -i $LONG -t 4 -c:v libx264 -preset veryfast -flags +ildct+ilme -g 60 -b:v 4M -pix_fmt yuv420p $O/vx_mbaff_bars_720.mp4
[ -f $O/vx_prores_720.mov ] || $F -f lavfi -i $LONG -frames:v 20 -c:v prores_ks -profile:v 2 -pix_fmt yuv422p10le $O/vx_prores_720.mov
for f in vx_hevc10_720.mp4 vx_vp9_720.mp4 vx_av1_720.mp4 vx_av1_10bit_720.mp4 vx_mbaff_720.mp4; do
    check "decode $f" python3 comparex.py decode $O/$f
    check "seek $f" python3 comparex.py seek $O/$f 40 7
done
for f in vx_vp9_720.mp4 vx_av1_720.mp4 vx_av1_10bit_720.mp4; do
    check "software decode $f" python3 comparex.py decode $O/$f --software
    check "software seek $f" python3 comparex.py seek $O/$f 40 7 --software
done
check "interlaced colour bars stay above 25 dB (known limit of the system's software H.264 decoder)" python3 comparex.py decode $O/vx_mbaff_bars_720.mp4 25
check "ProRes 422 at 720p within 60 dB of ffmpeg" python3 comparex.py decode $O/vx_prores_720.mov 60

echo "== videox: AV1 and VP9 at 1080p against ffmpeg's libsvtav1 and libvpx-vp9, same bitrate and speed setting"
VXN=60
[ -n "${QUICK:-}" ] || VXN=150
python3 comparex.py race $O/raw_1080.yuv $O $VXN
check "av1 1080p is no more than 4 dB under ffmpeg's libsvtav1 (its one-pass rate control varies by 2 to 3 dB between runs)" python3 comparex.py near $O/vx_race_av1.ivf $O/vx_race_ffmpeg_av1.ivf $O/raw_1080.yuv 1920 1080 4
check "vp9 1080p is no more than 1 dB under ffmpeg's libvpx-vp9" python3 comparex.py near $O/vx_race_vp9.ivf $O/vx_race_ffmpeg_vp9.ivf $O/raw_1080.yuv 1920 1080 1
rm -f $O/vx_race_*.ivf
