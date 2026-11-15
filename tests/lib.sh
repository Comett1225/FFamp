FFMPEG="${FFMPEG:-ffmpeg}"
FFPROBE="${FFPROBE:-ffprobe}"
export FFMPEG FFPROBE
A=../build/ffamp
O=out
SYSTEM="$(uname -s)"
case "$SYSTEM" in Darwin) ;; MINGW*|MSYS*|CYGWIN*) SYSTEM=Windows; A=../build/ffamp.exe ;; *) SYSTEM=Linux ;; esac
on_mac() { [ "$SYSTEM" = Darwin ]; }
command -v md5 > /dev/null || md5() { md5sum | cut -d' ' -f1; }
on_mac || stat() { if [ "$1" = -f ] && [ "$2" = %z ]; then command stat -c %s "$3"; else command stat "$@"; fi; }
APPLE_JUDGES=" ../build/avcheck ../build/audiocheck av_opens av_frames apple_frames apple_same apple_lossy apple_payload opens "
F="$FFMPEG -nostdin -hide_banner -v error -y"
case "$SYSTEM" in Darwin) FF_AAC=aac_at FF_H264="h264_videotoolbox -realtime 0 -profile:v high" ;; Windows) FF_AAC=aac_mf FF_H264=h264_mf ;; *) FF_AAC=aac FF_H264="libx264 -preset veryfast -profile:v high" ;; esac
failed=0

have_ffmpeg() { [ -z "${NOFFMPEG:-}" ] && command -v "$FFMPEG" > /dev/null; }

skip() { echo "SKIP  $1"; }

check() {
    if ! on_mac && { [[ "$APPLE_JUDGES" == *" $2 "* ]] || [[ "$1" == *Apple* ]] || [[ "$1" == *AVFoundation* ]] || [[ "$*" == *build/avcheck* ]] || [[ "$*" == *build/audiocheck* ]]; }; then skip "$1 (Apple's frameworks are the judge)"; return; fi
    if [[ "$*" == *build/fuzz_* ]] && [ ! -x ../build/fuzz_open ]; then skip "$1 (the fuzz targets are not built on this system)"; return; fi
    if "${@:2}"; then echo "PASS  $1"; else echo "FAIL  $1"; failed=1; fi
}

float_check() {
    if on_mac && [ "$(uname -m)" = arm64 ]; then check "$@"; else skip "$1 (checksum made on an arm64 Mac, where float decoders round differently; judged against ffmpeg instead)"; fi
}

clean() {
    [ -z "$("$FFPROBE" -v error "$1" 2>&1)" ] && [ -z "$("$FFMPEG" -nostdin -v error -i "$1" -f null - 2>&1)" ]
}

same_packets() {
    [ "$("$FFMPEG" -nostdin -v error -i "$1" -map 0 -c copy -f framemd5 - | grep -v '^#' | md5)" = "$("$FFMPEG" -nostdin -v error -i "$2" -map 0 -c copy -f framemd5 - | grep -v '^#' | md5)" ]
}

cmake -S .. -B ../build -G Ninja -DCMAKE_BUILD_TYPE=Release ${FFAMP_CMAKE:-} > /dev/null && cmake --build ../build > /dev/null || exit 1
if on_mac; then clang -fobjc-arc -O1 -w avcheck.m -o ../build/avcheck -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework Foundation -framework CoreGraphics || exit 1; fi
mkdir -p $O

if have_ffmpeg; then
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
fi
