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
check "BGRA to I420 is within one code value of the system converter (colour constant over each 2x2 block, where averaging and the system's point sampling agree)" $A pixels --size 640x360
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
check "ProRes against ffmpeg (VideoToolbox within 60 dB, the own decoder bit-exact)" python3 comparex.py decode $VX/prores_180p.mov $(on_mac && echo 60)
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
check "ProRes 422 at 720p against ffmpeg (VideoToolbox within 60 dB, the own decoder bit-exact)" python3 comparex.py decode $O/vx_prores_720.mov $(on_mac && echo 60)

echo "== videox: own ProRes and DV decoders, every profile against ffmpeg"
if on_mac; then
    skip "own ProRes and DV decoders against ffmpeg (VideoToolbox decodes them on the Mac)"
else
    PD="testsrc2=size=480x270:rate=24,noise=alls=12:allf=t"
    PA="$PD,format=yuva444p10le,geq=lum='lum(X,Y)':cb='cb(X,Y)':cr='cr(X,Y)':a='X*1023/W'"
    vx_make() { [ -f $O/$1 ] || $F -f lavfi -i "$2" "${@:3}" $O/$1; }
    vx_own() { $A videoinfo $O/$1 | grep -q "backend $2 "; }
    vx_make pd_proxy.mov $PD -frames:v 6 -c:v prores_ks -profile:v 0 -pix_fmt yuv422p10le
    vx_make pd_lt.mov $PD -frames:v 6 -c:v prores_ks -profile:v 1 -pix_fmt yuv422p10le
    vx_make pd_standard.mov $PD -frames:v 6 -c:v prores_ks -profile:v 2 -pix_fmt yuv422p10le
    vx_make pd_hq.mov $PD -frames:v 6 -c:v prores_ks -profile:v 3 -pix_fmt yuv422p10le
    vx_make pd_4444.mov $PD -frames:v 6 -c:v prores_ks -profile:v 4 -pix_fmt yuv444p10le
    vx_make pd_xq.mov $PD -frames:v 6 -c:v prores_ks -profile:v 5 -pix_fmt yuv444p10le
    vx_make pd_alpha.mov "$PA" -frames:v 4 -c:v prores_ks -profile:v 4 -pix_fmt yuva444p10le
    vx_make pd_alpha8.mov "$PA" -frames:v 4 -c:v prores_ks -profile:v 4 -alpha_bits 8 -pix_fmt yuva444p10le
    vx_make pd_odd.mov "testsrc2=size=202x118:rate=24,noise=alls=12:allf=t" -frames:v 5 -c:v prores_ks -profile:v 3 -pix_fmt yuv422p10le
    vx_make pd_tff.mov "$PD,setfield=tff" -frames:v 5 -flags +ildct -c:v prores_ks -profile:v 2 -pix_fmt yuv422p10le
    vx_make pd_bff.mov "$PD,setfield=bff" -frames:v 5 -flags +ildct -c:v prores_ks -profile:v 3 -pix_fmt yuv422p10le
    vx_make pd_odd_tff.mov "testsrc2=size=210x99:rate=24,setfield=tff" -frames:v 5 -flags +ildct -c:v prores_ks -profile:v 4 -pix_fmt yuv444p10le
    vx_make pd_aw.mov $PD -frames:v 5 -c:v prores_aw -profile:v 3 -pix_fmt yuv422p10le
    DN=",noise=alls=12:allf=t"
    vx_make pd_dv_pal.mov "testsrc2=size=720x576:rate=25$DN" -frames:v 5 -c:v dvvideo -pix_fmt yuv420p
    vx_make pd_dv_ntsc.mov "testsrc2=size=720x480:rate=30000/1001$DN" -frames:v 5 -c:v dvvideo -pix_fmt yuv411p
    vx_make pd_dv_pal_bff.avi "testsrc2=size=720x576:rate=25$DN,setfield=bff" -frames:v 5 -flags +ildct -c:v dvvideo -pix_fmt yuv420p
    vx_make pd_dv_ntsc_bff.avi "testsrc2=size=720x480:rate=30000/1001$DN,setfield=bff" -frames:v 5 -flags +ildct -c:v dvvideo -pix_fmt yuv411p
    vx_make pd_dv50_pal.mov "testsrc2=size=720x576:rate=25$DN" -frames:v 5 -c:v dvvideo -pix_fmt yuv422p
    vx_make pd_dv50_ntsc.avi "testsrc2=size=720x480:rate=30000/1001$DN" -frames:v 5 -c:v dvvideo -pix_fmt yuv422p
    vx_make pd_dvhd_1080i60.mov "testsrc2=size=1280x1080:rate=30000/1001$DN" -frames:v 3 -c:v dvvideo -pix_fmt yuv422p
    vx_make pd_dvhd_1080i50.mov "testsrc2=size=1440x1080:rate=25$DN" -frames:v 3 -c:v dvvideo -pix_fmt yuv422p
    vx_make pd_dvhd_1080i50.avi "testsrc2=size=1440x1080:rate=25$DN,setfield=tff" -frames:v 3 -flags +ildct -c:v dvvideo -pix_fmt yuv422p
    vx_make pd_dvhd_720p60.mov "testsrc2=size=960x720:rate=60000/1001$DN" -frames:v 3 -c:v dvvideo -pix_fmt yuv422p
    vx_make pd_dvhd_720p50.mov "testsrc2=size=960x720:rate=50$DN" -frames:v 3 -c:v dvvideo -pix_fmt yuv422p
    for f in pd_proxy.mov pd_lt.mov pd_standard.mov pd_hq.mov pd_4444.mov pd_xq.mov pd_alpha.mov pd_alpha8.mov pd_odd.mov pd_tff.mov pd_bff.mov pd_odd_tff.mov pd_aw.mov \
        pd_dv_pal.mov pd_dv_ntsc.mov pd_dv_pal_bff.avi pd_dv_ntsc_bff.avi pd_dv50_pal.mov pd_dv50_ntsc.avi pd_dvhd_1080i60.mov pd_dvhd_1080i50.mov pd_dvhd_1080i50.avi \
        pd_dvhd_720p60.mov pd_dvhd_720p50.mov; do
        case $f in pd_dv*) backend=dv ;; *) backend=prores ;; esac
        check "$f opens with the own $backend decoder" vx_own $f $backend
        check "decode $f bit-exact against ffmpeg" python3 comparex.py decode $O/$f
        check "random frames of $f equal the straight decode" python3 comparex.py selfseek $O/$f 8 3
        check "damaged copies of $f never crash" python3 comparex.py fuzz $O/$f 8 5
    done
    for f in pd_alpha.mov pd_alpha8.mov; do check "alpha of $f equal to ffmpeg's, at full and at half size" python3 formats_check.py veil $O/$f 480 270 -an; done
    check "4:1:1 DV as BGRA against ffmpeg's converter" python3 comparex.py sdr $O/pd_dv_ntsc.mov 0.1 0.99
    $A decode $O/pd_dv_ntsc.mov --bgra > $O/pd_ref.bgra
    $A frames-out $O/pd_dv_ntsc.mov --format bgra > $O/pd_a.raw
    check "the frame reader gives 4:1:1 DV as our decoder's BGRA" cmp -s $O/pd_ref.bgra $O/pd_a.raw
    $F -i $O/pd_dv_ntsc.mov -frames:v 1 -pix_fmt yuv420p -f rawvideo $O/pd_ref.i420
    $A frames-out $O/pd_dv_ntsc.mov --format i420 --count 1 > $O/pd_a.raw
    check "the frame reader gives 4:1:1 DV as I420 close to ffmpeg's" python3 capturecheck.py psnr $O/pd_ref.i420 $O/pd_a.raw 40
    $F -i $O/pd_dv_ntsc.mov -frames:v 1 -vf scale=360:240:flags=bicubic -pix_fmt rgba -f rawvideo $O/pd_ref.rgba
    $A frames-out $O/pd_dv_ntsc.mov --size 360x240 --format rgba --count 1 > $O/pd_a.raw
    check "a 4:1:1 DV thumbnail at half size is close to ffmpeg's" python3 capturecheck.py psnr $O/pd_ref.rgba $O/pd_a.raw 30
    rm -f $O/pd_ref.* $O/pd_a.raw
fi

echo "== videox: MPEG-1/2 through libmpeg2 and Motion JPEG through libjpeg-turbo against ffmpeg"
if [ "$SYSTEM" != Linux ]; then
    skip "libmpeg2 and libjpeg-turbo against ffmpeg (system decoders on the Mac and Windows)"
else
    MD="testsrc2=size=720x576:rate=25,noise=alls=12:allf=t"
    MH="testsrc2=size=1920x1080:rate=25,noise=alls=12:allf=t"
    mv_make() { [ -f $O/$1 ] || $F -f lavfi -i "$2" "${@:3}" $O/$1; }
    mv_own() { $A videoinfo $O/$1 | grep -q "backend $2 "; }
    mv_cut() { $A decode $O/mv_cut.mkv > /dev/null && [ "$($A frames $O/mv_cut.mkv --list 0 | md5)" = "$($A frames $O/mv_cut.mkv --list 3 | md5)" ]; }
    mv_colour() { $A videoinfo $O/mv_mpeg2_colour.avi | grep -q "size 720x576 .* primaries 1 transfer 1 matrix 1 "; }
    mv_make mv_mpeg1.mov "$MD" -frames:v 30 -c:v mpeg1video -q:v 4 -g 12 -bf 2
    mv_make mv_mpeg1_odd.avi "testsrc2=size=203x117:rate=25,noise=alls=12:allf=t" -frames:v 20 -c:v mpeg1video -q:v 4 -g 9 -bf 1
    mv_make mv_mpeg2.mp4 "$MD" -frames:v 30 -c:v mpeg2video -q:v 4 -g 12 -bf 2
    mv_make mv_mpeg2_open.mov "$MD" -frames:v 40 -c:v mpeg2video -q:v 4 -g 12 -bf 3 -intra_vlc 1 -non_linear_quant 1 -qmax 28
    mv_make mv_mpeg2_tff.mkv "$MD,setfield=tff" -frames:v 30 -c:v mpeg2video -q:v 4 -g 12 -bf 2 -flags +ildct+ilme -alternate_scan 1
    mv_make mv_mpeg2_bff.avi "$MD,setfield=bff" -frames:v 30 -c:v mpeg2video -q:v 4 -g 12 -bf 2 -flags +ildct+ilme
    mv_make mv_mpeg2_colour.avi "$MD,setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709" -frames:v 10 -c:v mpeg2video -q:v 4 -g 5
    mv_make mv_xdcam422.mov "$MH,setfield=tff" -frames:v 14 -c:v mpeg2video -pix_fmt yuv422p -q:v 4 -g 12 -bf 2 -flags +ildct+ilme
    mv_make mv_imx.mov "testsrc2=size=720x608:rate=25,noise=alls=12:allf=t,setfield=tff" -frames:v 10 -c:v mpeg2video -pix_fmt yuv422p -g 1 -bf 0 -dc 10 -intra_vlc 1 \
        -non_linear_quant 1 -qmax 28 -q:v 2 -flags +ildct -tag:v mx5p
    for order in top bottom; do
        [ -f $O/mv_fields_$order.mp4 ] && continue
        $F -f lavfi -i "$MD,setfield=$([ $order = top ] && echo tff || echo bff),separatefields" -frames:v 16 -c:v mpeg2video -g 1 -bf 0 -q:v 3 -f mpeg2video $O/mv_half.m2v
        python3 mpeg_craft.py fields $O/mv_half.m2v $O/mv_fields.m2v $order
        $F -fflags +genpts -r 25 -i $O/mv_fields.m2v -c copy $O/mv_fields_$order.mp4
        rm -f $O/mv_half.m2v $O/mv_fields.m2v
    done
    if [ ! -f $O/mv_joined.mkv ]; then
        $F -f lavfi -i "$MD" -frames:v 10 -c:v mpeg2video -q:v 4 -g 5 -bf 0 -f mpeg2video $O/mv_a.m2v
        $F -f lavfi -i "testsrc2=size=480x270:rate=25,noise=alls=12:allf=t" -frames:v 10 -c:v mpeg2video -q:v 4 -g 5 -bf 0 -f mpeg2video $O/mv_b.m2v
        cat $O/mv_a.m2v $O/mv_b.m2v > $O/mv_ab.m2v
        for p in a b ab; do $F -fflags +genpts -r 25 -i $O/mv_$p.m2v -c copy $O/mv_$p.mkv; done
        mv $O/mv_ab.mkv $O/mv_joined.mkv
        rm -f $O/mv_*.m2v
    fi
    if [ ! -f $O/mv_cut.mkv ]; then
        $F -ss 0.5 -i $O/mv_mpeg2_open.mov -c copy -f mpeg2video $O/mv_cut.m2v
        $F -fflags +genpts -r 25 -i $O/mv_cut.m2v -c copy $O/mv_cut.mkv
        rm -f $O/mv_cut.m2v
    fi
    mv_make mv_mjpeg420.avi "$MD" -frames:v 10 -c:v mjpeg -q:v 4 -pix_fmt yuvj420p
    mv_make mv_mjpeg422.mov "$MD" -frames:v 10 -c:v mjpeg -q:v 4 -pix_fmt yuvj422p
    mv_make mv_mjpeg444.avi "$MD" -frames:v 10 -c:v mjpeg -q:v 4 -pix_fmt yuvj444p
    mv_make mv_mjpeg444.mov "$MD" -frames:v 10 -c:v mjpeg -q:v 2 -pix_fmt yuvj444p
    mv_make mv_mjpeg_odd.mov "testsrc2=size=203x117:rate=25,noise=alls=12:allf=t" -frames:v 10 -c:v mjpeg -q:v 4 -pix_fmt yuvj420p
    if [ ! -f $O/mv_mjpeg_bare.avi ]; then
        $F -f lavfi -i "$MD" -frames:v 10 -c:v mjpeg -q:v 4 -huffman default -pix_fmt yuvj422p -f mjpeg $O/mv_tables.mjpeg
        python3 mpeg_craft.py nohuffman $O/mv_tables.mjpeg $O/mv_bare.mjpeg
        $F -f mjpeg -framerate 25 -i $O/mv_bare.mjpeg -c copy $O/mv_mjpeg_bare.avi
        rm -f $O/mv_tables.mjpeg $O/mv_bare.mjpeg
    fi
    while read f floor; do
        case $f in *mjpeg*) backend=libjpeg-turbo ;; *) backend=libmpeg2 ;; esac
        check "$f opens with $backend" mv_own $f $backend
        check "decode $f against ffmpeg (other IDCT, within $floor dB)" python3 comparex.py decode $O/$f $floor
        check "random frames of $f equal the straight decode" python3 comparex.py selfseek $O/$f 10 3
        check "damaged copies of $f never crash" python3 comparex.py fuzz $O/$f 8 5
    done <<TABLE
mv_mpeg1.mov 55
mv_mpeg1_odd.avi 55
mv_mpeg2.mp4 55
mv_mpeg2_open.mov 55
mv_mpeg2_tff.mkv 55
mv_mpeg2_bff.avi 55
mv_mpeg2_colour.avi 55
mv_xdcam422.mov 55
mv_imx.mov 55
mv_fields_top.mp4 55
mv_fields_bottom.mp4 55
mv_a.mkv 55
mv_b.mkv 55
mv_mjpeg420.avi 60
mv_mjpeg422.mov 60
mv_mjpeg444.avi 60
mv_mjpeg444.mov 60
mv_mjpeg_odd.mov 60
mv_mjpeg_bare.avi 60
TABLE
    check "the MPEG-2 colour comes from the sequence display extension" mv_colour
    check "a cut that starts inside an open GOP decodes, its first B-pictures shown as the key picture they lead to" mv_cut
    check "random frames of that cut equal the straight decode" python3 comparex.py selfseek $O/mv_cut.mkv 10 3
    check "a picture size change in the stream gives each part as its own file does" [ "$($A decode $O/mv_joined.mkv | md5)" = "$( { $A decode $O/mv_a.mkv; $A decode $O/mv_b.mkv; } | md5)" ]
fi

echo "== videox: AV1 and VP9 at 1080p against ffmpeg's libsvtav1 and libvpx-vp9, same bitrate and speed setting"
VXN=60
[ -n "${QUICK:-}" ] || VXN=150
python3 comparex.py race $O/raw_1080.yuv $O $VXN
check "av1 1080p is no more than 4 dB under ffmpeg's libsvtav1 (its one-pass rate control varies by 2 to 3 dB between runs)" python3 comparex.py near $O/vx_race_av1.ivf $O/vx_race_ffmpeg_av1.ivf $O/raw_1080.yuv 1920 1080 4
check "vp9 1080p is no more than 1 dB under ffmpeg's libvpx-vp9" python3 comparex.py near $O/vx_race_vp9.ivf $O/vx_race_ffmpeg_vp9.ivf $O/raw_1080.yuv 1920 1080 1
rm -f $O/vx_race_*.ivf
