echo "== fixtures: decode, transcode and remux without FFmpeg"
FX=fixtures/video
fixture_decode() { [ "$($A decode "$FX/$1" | md5)" = "$(grep " $1\$" $FX/decoded.md5 | cut -d' ' -f1)" ]; }
for f in h264_270p.mp4 hevc_270p.mp4; do
    check "decode $f matches the checked checksum" fixture_decode $f
    $A transcode $FX/$f $O/fx_transcode_$f --codec hevc --bitrate 1000000 2> /dev/null
    check "transcode $f opens in AVFoundation" ../build/avcheck $O/fx_transcode_$f
    $A remux $FX/$f $O/fx_remux_$f
    check "remux $f opens in AVFoundation" ../build/avcheck $O/fx_remux_$f
    check "remux $f decodes to the same frames" [ "$($A decode $O/fx_remux_$f | md5)" = "$($A decode $FX/$f | md5)" ]
done
have_ffmpeg || return 0
echo "== decode: every frame against ffmpeg"
for f in h264_720.mp4 hevc_720.mp4 frag_h264.mp4 hevc_720.mov h264_1080_60s.mp4 hevc_1080_60s.mp4; do check "decode $f" python3 compare.py decode $O/$f; done

echo "== seek: 50 random frames against ffmpeg"
for f in h264_720.mp4 hevc_720.mp4 h264_1080_60s.mp4 hevc_1080_60s.mp4; do check "seek $f" python3 compare.py seek $O/$f 50 7; done
if [ "$SYSTEM" = Windows ]; then
    for f in h264_720.mp4 h264_1080_60s.mp4; do
        check "decode $f with FFamp's own decoder first (FFAMP_OWN_H264)" env FFAMP_OWN_H264=1 python3 compare.py decode $O/$f
        check "seek $f with FFamp's own decoder first (FFAMP_OWN_H264)" env FFAMP_OWN_H264=1 python3 compare.py seek $O/$f 50 7
    done
fi
if ! on_mac; then
    for f in h264_720.mp4 hevc_720.mp4; do
        check "decode $f with the system decoder failing at sample 40 (FFAMP_FAIL_SYSTEM_VIDEO): the software decoder takes over" env FFAMP_FAIL_SYSTEM_VIDEO=40 python3 compare.py decode $O/$f
        check "seek $f with the system decoder failing at sample 40 (FFAMP_FAIL_SYSTEM_VIDEO)" env FFAMP_FAIL_SYSTEM_VIDEO=40 python3 compare.py seek $O/$f 50 7
    done
fi
python3 compare.py cliseek $O/h264_1080_60s.mp4 20 7
python3 compare.py cliseek $O/hevc_1080_60s.mp4 20 7

echo "== encode: VideoToolbox + AAC into our MP4"
for c in h264 hevc; do
    $A encode $O/enc_$c.mp4 --video $O/raw_1080.yuv --size 1920x1080 --fps 30 --codec $c --bitrate 8000000 --audio $O/pcm.raw
    check "enc_$c.mp4 has no ffprobe or decode errors" clean $O/enc_$c.mp4
    check "enc_$c.mp4 opens in AVFoundation (QuickTime's engine)" ../build/avcheck $O/enc_$c.mp4
    check "enc_$c.mp4 audio matches the source" python3 compare.py audio $O/enc_$c.mp4 $O/pcm.raw 48000 2
    check "enc_$c.mp4 has 300 frames" [ "$("$FFPROBE" -v error -select_streams v:0 -count_frames -show_entries stream=nb_read_frames -of csv=p=0 $O/enc_$c.mp4 | tr -d ,)" = 300 ]
done

echo "== Matroska: our writer, Opus through libopus when it is installed"
if $A encode $O/enc_opus.mkv --video $O/raw_1080.yuv --size 1920x1080 --fps 30 --codec h264 --bitrate 8000000 --audio $O/pcm.raw --acodec opus 2> /dev/null; then
    check "enc_opus.mkv has no ffprobe or decode errors" clean $O/enc_opus.mkv
    check "enc_opus.mkv audio matches the source" python3 compare.py audio $O/enc_opus.mkv $O/pcm.raw 48000 2
    check "enc_opus.mkv seeks to an exact frame" python3 compare.py fileseek $O/enc_opus.mkv 225
else
    echo "SKIP  Opus (libopus not found when building)"
fi
$A encode $O/enc_aac.mkv --video $O/raw_1080.yuv --size 1920x1080 --fps 30 --codec hevc --bitrate 6000000 --audio $O/pcm.raw
check "enc_aac.mkv has no ffprobe or decode errors" clean $O/enc_aac.mkv
check "enc_aac.mkv audio matches the source" python3 compare.py audio $O/enc_aac.mkv $O/pcm.raw 48000 2
check "enc_aac.mkv seeks to an exact frame" python3 compare.py fileseek $O/enc_aac.mkv 225

echo "== stream copy and compress"
for f in h264_720 hevc_720; do
    $A remux $O/$f.mp4 $O/remux_$f.mp4
    check "remux $f keeps every packet" same_packets $O/$f.mp4 $O/remux_$f.mp4
    check "remux $f opens in AVFoundation" ../build/avcheck $O/remux_$f.mp4
done
$A remux $O/frag_h264.mp4 $O/remux_frag.mp4
check "fragmented MP4 turned into a plain MP4" clean $O/remux_frag.mp4
$A transcode $O/h264_720.mp4 $O/compress.mp4 --codec hevc --bitrate 2000000
check "compress output is clean" clean $O/compress.mp4
check "compress output opens in AVFoundation" ../build/avcheck $O/compress.mp4

