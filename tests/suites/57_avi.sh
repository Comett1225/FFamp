echo "== AVI: a corpus made by ffmpeg at test time, judged against ffmpeg frame by frame and sample by sample"
have_ffmpeg || { skip "the AVI corpus (ffmpeg makes it)"; return 0; }
AV=$O/avi
python3 avi_make.py $AV > /dev/null
AV_FUZZ=12
[ -n "${QUICK:-}" ] && AV_FUZZ=4
av_has() { [ -f $AV/$1 ]; }
av_opens() { $A videoinfo $AV/$1 > /dev/null 2>&1; }
av_packets() { $A packets $AV/$1 | grep -v -e '^format' -e '^tag'; }
av_same() { [ "$(av_packets $1 | md5)" = "$(av_packets $2 | md5)" ]; }
av_payload() { [ "$($A packets $AV/$1 --track 1 --raw | md5)" = "$($FFMPEG -nostdin -v error -i $AV/$1 -map 0:v:0 -c copy -f data - | md5)" ]; }

echo "== AVI: pictures against ffmpeg"
while read f linux windows mac seek; do
    if ! av_has $f.avi; then skip "$f.avi (this ffmpeg cannot write it)"; continue; fi
    rule=$linux
    [ "$SYSTEM" = Windows ] && rule=$windows
    on_mac && rule=$mac
    case $rule in
        gray|rgb555le) check "decode $f.avi to the planes ffmpeg gives as $rule" python3 avi_check.py planes $AV/$f.avi $rule ;;
        bgra*) check "decode $f.avi as BGRA within ${rule#bgra} dB of ffmpeg" python3 formats_check.py bgra $AV/$f.avi ${rule#bgra} ;;
        *) check "decode $f.avi$([ $rule = 0 ] && echo ' bit-exact' || echo " within $rule dB")" python3 comparex.py decode $AV/$f.avi $([ $rule = 0 ] || echo $rule) ;;
    esac
    check "frame times of $f.avi follow ffprobe's" python3 avi_check.py times $AV/$f.avi
    if [ $seek = ff ] && [ $rule = 0 ]; then
        check "random frames of $f.avi equal ffmpeg's" python3 comparex.py seek $AV/$f.avi 12 3
    else
        check "random frames of $f.avi equal the straight decode" python3 comparex.py selfseek $AV/$f.avi 12 3
    fi
done <<TABLE
avi_mjpeg_pcm 60 60 60 self
avi_mjpeg422_u8 60 bgra30 bgra30 self
avi_xvid_mp3 60 60 45 self
avi_divx_mp3vbr 60 60 45 self
avi_dx50_ac3 60 60 45 self
avi_fmp4_mp2 60 60 45 self
avi_div3_mp3 80 80 0 ff
avi_mp42_s24 80 80 0 ff
avi_h264_aac 0 0 0 ff
avi_h264_ntsc_ac3 0 0 0 ff
avi_h264_avc1 0 0 0 ff
avi_mpeg2_mp2 55 55 48 self
avi_mpeg1_pcm 55 55 48 self
avi_bgr24_pcm 0 0 bgra45 ff
avi_bgr24_odd 0 0 bgra45 ff
avi_bgra_f32 0 0 bgra45 ff
avi_rgb555_alaw rgb555le rgb555le bgra40 self
avi_pal8_mulaw 0 0 bgra45 ff
avi_yuy2_ima 0 0 0 ff
avi_uyvy_msadpcm 0 0 0 ff
avi_i420_pcm 0 0 0 ff
avi_yv12_pcm 0 0 0 ff
avi_gray_pcm gray gray gray self
avi_cinepak_ima 0 0 bgra45 ff
avi_cinepak_small 0 0 bgra45 ff
avi_mjpeg_wma 60 60 60 self
avi_dv_pcm48 0 0 33 self
avi_dv_type1 0 0 33 self
avi_mjpeg_adts 60 60 60 self
avi_vfr_dropped 60 60 45 self
TABLE
check "Cinepak key frames are its all-intra pictures, as the encoder's index marks them" [ "$($A packets $AV/avi_cinepak_ima.avi --track 1 | grep -c ' K ')" = "$($FFPROBE -v error -select_streams v -show_entries packet=flags -of csv=p=0 $AV/avi_cinepak_ima.avi | grep -c K)" ]
check "H.264 stored as avc1 (length-prefixed) keeps ffmpeg's packet bytes" av_payload avi_h264_avc1.avi
for f in huffyuv utvideo magicyuv; do
    av_has avi_${f}_pcm.avi || { skip "$f (this ffmpeg cannot write it)"; continue; }
    check "$f in AVI: every picture packet as ffmpeg reads it" av_payload avi_${f}_pcm.avi
    if av_opens avi_${f}_pcm.avi; then check "decode avi_${f}_pcm.avi bit-exact" python3 comparex.py decode $AV/avi_${f}_pcm.avi; else skip "decode $f (its decoder is not on this branch)"; fi
done

echo "== AVI: sound against ffmpeg"
while read f linux windows mac; do
    av_has $f.avi || { skip "$f.avi (this ffmpeg cannot write it)"; continue; }
    least=$linux
    [ "$SYSTEM" = Windows ] && least=$windows
    on_mac && least=$mac
    case $least in
        exact) check "sound of $f.avi bit-exact against ffmpeg" python3 audio.py against $AV/$f.avi 200 0 0 exact ;;
        wma*) check "sound of $f.avi against ffmpeg after the decoder's hidden start" python3 wma_check.py against $AV/$f.avi ${least#wma} ;;
        *) check "sound of $f.avi within $least dB of ffmpeg, same length" python3 audio.py against $AV/$f.avi $least 0 0 ;;
    esac
done <<TABLE
avi_mjpeg_pcm exact exact exact
avi_mjpeg422_u8 exact exact exact
avi_mp42_s24 exact exact exact
avi_bgra_f32 exact exact exact
avi_rgb555_alaw exact exact exact
avi_pal8_mulaw exact exact exact
avi_yuy2_ima exact exact exact
avi_uyvy_msadpcm exact exact exact
avi_xvid_mp3 100 100 55
avi_divx_mp3vbr 100 100 55
avi_fmp4_mp2 70 70 70
avi_dx50_ac3 120 120 50
avi_h264_aac 100 45 45
avi_mjpeg_wma wma120 wma120 wma120
avi_dv_pcm48 exact exact exact
avi_dv_type1 exact exact exact
avi_dv_type1_12bit exact exact exact
avi_mjpeg_eac3 120 120 50
avi_mjpeg_adts 100 45 45
TABLE

on_mac || check "sound of avi_h264_aac.avi through FFamp's own AAC decoder within 135 dB of ffmpeg" env FFAMP_NO_SYSTEM_AUDIO=1 python3 audio.py against $AV/avi_h264_aac.avi 135 0 0
check "AAC with ADTS headers in AVI is cut into raw AAC frames with its AudioSpecificConfig" [ "$($A packets $AV/avi_mjpeg_adts.avi --track 2 | sed -n 2p | cut -d" " -f4-10)" = "mp4a timescale 44100 start 0 end -1" ]

echo "== AVI: indexes, chunk layout and timing"
for v in noindex shortidx absolute unpadded unpadded_idx1 junk rec garbage split split_noindex nokeys; do
    check "avs_$v.avi gives the packets of the same file with a plain idx1" av_same avs_$v.avi avs_idx1.avi
done
check "a file with no index gives the packets of its OpenDML original (Cinepak, key frames from the pictures)" av_same avs_cinepak_noindex.avi avi_cinepak_ima.avi
check "a file with no index gives the packets of its OpenDML original (H.264)" av_same avs_h264_noindex.avi avi_h264_aac.avi
check "an idx1 with no key flags gives the packets of the original (DivX 3, key frames from the pictures)" av_same avs_div3_nokeys.avi avi_div3_mp3.avi
check "unpadded odd-sized raw pictures without an index give the original packets" av_same avs_bgr24_unpadded.avi avi_bgr24_odd.avi
check "all pictures, then all sound (not interleaved) gives the original packets" av_same avs_mjpeg_split.avi avi_mjpeg_pcm.avi
check "dropped-frame chunks read the same with and without an index" av_same avs_dropped.avi avs_dropped_noindex.avi
for f in avs_dropped avs_dropped_noindex avs_scale avs_garbage avs_unpadded; do check "frame times of $f.avi follow ffprobe's" python3 avi_check.py times $AV/$f.avi; done
check "decode avs_dropped.avi within 60 dB of ffmpeg" python3 comparex.py decode $AV/avs_dropped.avi 60
check "decode avs_garbage.avi within 60 dB of ffmpeg" python3 comparex.py decode $AV/avs_garbage.avi 60
check "decode the cut-off avs_cut.avi (the cut picture may differ)" python3 comparex.py decode $AV/avs_cut.avi 20
AV_MP3=100
on_mac && AV_MP3=55
check "sound of the cut-off avs_cut.avi within $AV_MP3 dB of ffmpeg, same length" python3 audio.py against $AV/avs_cut.avi $AV_MP3 0 0
check "sound of avs_unpadded.avi within $AV_MP3 dB of ffmpeg, same length" python3 audio.py against $AV/avs_unpadded.avi $AV_MP3 0 0
check "a sound stream with a start delay starts where ffprobe's packets start" python3 avi_check.py starts $AV/avs_audio_start.avi
check "damaged AVI files never crash the readers" python3 fuzz.py $O/av_fuzz.bin $AV_FUZZ $AV/avi_cinepak_ima.avi $AV/avi_h264_avc1.avi $AV/avi_mjpeg_wma.avi $AV/avi_uyvy_msadpcm.avi $AV/avs_rec.avi $AV/avs_dropped.avi
for f in avi_cinepak_ima avi_pal8_mulaw avi_h264_avc1 avi_yuy2_ima; do check "damaged copies of $f.avi decode or fail cleanly" python3 comparex.py fuzz $AV/$f.avi $AV_FUZZ 7; done

if [ -n "${QUICK:-}" ] || on_mac; then
    skip "AVI files over 1 GB and 4 GB (left out on the Mac and in quick runs)"
    return 0
fi
echo "== AVI: over 4 GB (OpenDML, several RIFF parts, 64-bit index bases), with and without its OpenDML index"
python3 avi_make.py $AV 4.3 > /dev/null
for f in avb_4.3g avb_4.3g_idx1; do
    check "$f.avi: frame count and frames across the file exact against ffmpeg" python3 avi_check.py far $AV/$f.avi 10 5 yuv422p
    check "$f.avi: sound length equals ffmpeg's decode" [ "$($A adecode $AV/$f.avi --frames 0 2>&1)" = "48000 Hz, 1 channels, $(($($FFMPEG -nostdin -v error -i $AV/$f.avi -vn -f s16le - | wc -c) / 2)) frames" ]
done
rm -f $AV/avb_*
