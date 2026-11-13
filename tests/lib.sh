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

