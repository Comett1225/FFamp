#!/bin/bash
set -u
cd "$(dirname "$0")"
FFMPEG="${FFMPEG:-ffmpeg}"
FFPROBE="${FFPROBE:-ffprobe}"
export FFMPEG FFPROBE
A=../build/amedia
O=out
F="$FFMPEG -nostdin -hide_banner -v error -y"
failed=0

check() {
    if "${@:2}"; then echo "PASS  $1"; else echo "FAIL  $1"; failed=1; fi
}

clean() {
    [ -z "$("$FFPROBE" -v error "$1" 2>&1)" ] && [ -z "$("$FFMPEG" -nostdin -v error -i "$1" -f null - 2>&1)" ]
}

same_packets() {
    [ "$("$FFMPEG" -nostdin -v error -i "$1" -map 0 -c copy -f framemd5 - | grep -v '^#' | md5)" = "$("$FFMPEG" -nostdin -v error -i "$2" -map 0 -c copy -f framemd5 - | grep -v '^#' | md5)" ]
}

cmake -S .. -B ../build -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null && cmake --build ../build > /dev/null || exit 1
clang -fobjc-arc -O1 -w avcheck.m -o ../build/avcheck -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework Foundation -framework CoreGraphics || exit 1
mkdir -p $O

SRC="testsrc2=size=1280x720:rate=30"
TONE="sine=frequency=440:sample_rate=48000"
[ -f $O/h264_720.mp4 ] || $F -f lavfi -i $SRC -f lavfi -i $TONE -t 10 -c:v libx264 -preset medium -g 60 -pix_fmt yuv420p -c:a aac -b:a 128k $O/h264_720.mp4
[ -f $O/hevc_720.mp4 ] || $F -f lavfi -i $SRC -f lavfi -i $TONE -t 10 -c:v libx265 -preset fast -x265-params log-level=error -tag:v hvc1 -pix_fmt yuv420p -c:a aac -b:a 128k $O/hevc_720.mp4
BIG="testsrc2=size=1920x1080:rate=30,noise=alls=10:allf=t"
[ -f $O/h264_1080_60s.mp4 ] || $F -f lavfi -i "$BIG" -f lavfi -i $TONE -t 60 -c:v libx264 -preset veryfast -b:v 8M -g 60 -pix_fmt yuv420p -c:a aac -b:a 128k -ac 2 $O/h264_1080_60s.mp4
[ -f $O/hevc_1080_60s.mp4 ] || $F -f lavfi -i "$BIG" -f lavfi -i $TONE -t 60 -c:v libx265 -preset veryfast -b:v 5M -x265-params log-level=error -tag:v hvc1 -pix_fmt yuv420p -c:a aac -b:a 128k -ac 2 $O/hevc_1080_60s.mp4
$F -i $O/h264_720.mp4 -c copy -movflags frag_keyframe+empty_moov+default_base_moof $O/frag_h264.mp4
$F -i $O/hevc_720.mp4 -c copy -f mov $O/hevc_720.mov
$F -f lavfi -i testsrc2=size=1920x1080:rate=30 -frames:v 300 -pix_fmt yuv420p -f rawvideo $O/raw_1080.yuv
$F -f lavfi -i $TONE -t 10 -ac 2 -f s16le $O/pcm.raw

echo "== decode: every frame against ffmpeg"
for f in h264_720.mp4 hevc_720.mp4 frag_h264.mp4 hevc_720.mov h264_1080_60s.mp4 hevc_1080_60s.mp4; do check "decode $f" python3 compare.py decode $O/$f; done

echo "== seek: 50 random frames against ffmpeg"
for f in h264_720.mp4 hevc_720.mp4 h264_1080_60s.mp4 hevc_1080_60s.mp4; do check "seek $f" python3 compare.py seek $O/$f 50 7; done
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

echo "== speed"
python3 bench.py decode $O/h264_1080_60s.mp4 1800
python3 bench.py decode $O/hevc_1080_60s.mp4 1800
python3 bench.py encode $O/raw_1080.yuv $O/pcm.raw $O 300
python3 bench.py transcode $O/h264_1080_60s.mp4 $O 1800

rm -f $O/raw_1080.yuv $O/bench_*.mp4
[ $failed = 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $failed
