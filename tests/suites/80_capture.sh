echo "== capture: frame reader, no FFmpeg needed"
CF=fixtures/capture
CV=fixtures/video
CP=fixtures/probe
C="python3 capturecheck.py"
cap_fails() { ! "$@" > /dev/null 2>&1 && [ $? -lt 2 ]; }
cap_refuses() { "$@" > /dev/null 2>&1; [ $? = 1 ]; }
cap_bytes() { [ "$(wc -c < "$1" | tr -d ' ')" = "$2" ]; }
cap_quiet() { "$@" 2> /dev/null; }
cap_info() { $A frames-out "$1" --info ${3:-} | grep -q "$2"; }

for f in h264_270p.mp4 hevc_270p.mp4 vp9_270p.mp4 av1_270p.mp4 hdr_pq_270p.mp4 hevc10_270p.mp4 prores_180p.mov; do
    $A decode $CV/$f --bgra > $O/cap_ref.bgra
    $A frames-out $CV/$f --format rgba > $O/cap_a.raw
    check "rgba frames of $f equal our decoder's picture" $C swapped $O/cap_ref.bgra $O/cap_a.raw rgba
    $A frames-out $CV/$f --format rgb24 > $O/cap_a.raw
    check "rgb24 frames of $f equal our decoder's picture" $C swapped $O/cap_ref.bgra $O/cap_a.raw rgb24
    $A frames-out $CV/$f --format bgra > $O/cap_a.raw
    check "bgra frames of $f equal our decoder's picture" cmp -s $O/cap_ref.bgra $O/cap_a.raw
done
for f in h264_270p.mp4 hevc_270p.mp4 vp9_270p.mp4 av1_270p.mp4 h264_mbaff_272p.mp4; do
    $A decode $CV/$f > $O/cap_ref.yuv
    $A frames-out $CV/$f --format i420 > $O/cap_a.raw
    check "i420 frames of $f equal our decoder's planes" cmp -s $O/cap_ref.yuv $O/cap_a.raw
done
check "random frames by number and by time equal the straight run (rgba)" $C randomly $CV/h264_270p.mp4 480x270 rgba 30
check "random frames equal the straight run when scaled (i420 320x180)" $C randomly $CV/hevc_270p.mp4 320x180 i420 30
check "random frames equal the straight run for VP9 (rgb24 240x136)" $C randomly $CV/vp9_270p.mp4 240x136 rgb24 24
check "a steady 15 fps takes every second picture" $C steady $CV/h264_270p.mp4 15360 512 15 0 9 1
check "a steady 60 fps shows every picture twice" $C steady $CV/h264_270p.mp4 15360 512 60 0.5 0.75 1
check "a steady 23.976 fps from a start time follows the nearest-time rule" $C steady $CV/h264_270p.mp4 15360 512 23.976 0.21 1.3 1
check "a file with one track starts its steady run at the first picture's slot" $C steady $CP/ntsc.mp4 30000 1001 50 0.05 0.9 0
check "a steady 12.5 fps stops at the asked length" $C steady $CV/hevc_270p.mp4 15360 512 12.5 0.1 1.0 1

$A frames-out $CV/h264_270p.mp4 --size 240x136 --format rgba --start 0.666 --count 1 > $O/cap_a.raw
check "scaling down matches the checked bicubic reference" $C psnr $CF/down_240x136.rgba $O/cap_a.raw 45
$A frames-out $CV/h264_270p.mp4 --size 560x316 --format i420 --start 0.333 --count 1 > $O/cap_a.raw
check "scaling up matches the checked bicubic reference" $C psnr $CF/up_560x316.i420 $O/cap_a.raw 45
$A frames-out $CP/rot90.mp4 --size 120x214 --format rgb24 --count 1 > $O/cap_a.raw
check "a rotated file comes out upright and scaled like the checked reference" $C psnr $CF/turned_120x214.rgb $O/cap_a.raw 40
$A frames-out $CV/hevc10_270p.mp4 --size 320x180 --format i420 --start 0.2 --count 1 > $O/cap_a.raw
check "a 10-bit file scales to 8-bit planes like the checked reference" $C psnr $CF/deep_320x180.i420 $O/cap_a.raw 45
for turn in 90 180 270; do
    $A decode $CP/rot$turn.mp4 --bgra > $O/cap_ref.bgra
    $A frames-out $CP/rot$turn.mp4 --format bgra --no-rotate > $O/cap_a.raw
    check "rot$turn without rotation equals the stored picture" cmp -s $O/cap_ref.bgra $O/cap_a.raw
    $A frames-out $CP/rot$turn.mp4 --format bgra > $O/cap_b.raw
    check "rot$turn comes out turned by its stored rotation" $C turned $O/cap_a.raw $O/cap_b.raw 160 90 $($A frames-out $CP/rot$turn.mp4 --info | cut -d' ' -f6)
done
check "a rotated file reports its upright size" cap_info $CP/rot90.mp4 "size 90x160 source 160x90"
check "a rotated file reports its stored size when rotation is off" cap_info $CP/rot90.mp4 "size 160x90" --no-rotate
check "frame times start where the file starts (video delayed by an edit list)" [ "$($A frames-out $CP/edits.mp4 --start 0 --count 1 --format i420 | md5)" = "$($A frames-out $CP/edits.mp4 --frame 0 --count 1 --format i420 | md5)" ]
check "real-time pacing spreads 22 frames of a 30 fps file over 0.7 s" $C paced 0.68 4 frames-out $CV/h264_270p.mp4 --size 64x36 --count 22 --realtime
check "real-time pacing follows a steady rate too" $C paced 0.55 4 frames-out $CV/h264_270p.mp4 --size 64x36 --fps 10 --count 7 --realtime
check "a start time past the end gives no frames" [ "$($A frames-out $CV/h264_270p.mp4 --start 99 | wc -c | tr -d ' ')" = 0 ]
$A frames-out $CV/h264_270p.mp4 --size 1920x1080 --format rgba --count 2 > $O/cap_a.raw
check "1080p output has the right size" cap_bytes $O/cap_a.raw $((1920 * 1080 * 4 * 2))
$A frames-out $CV/hdr_pq_270p.mp4 --size 213x121 --format rgb24 --count 2 > $O/cap_a.raw
check "odd sizes from an HDR file have the right size" cap_bytes $O/cap_a.raw $((213 * 121 * 3 * 2))
$A frames-out $CV/h264_270p.mp4 --size 2x2 --format i420 --count 1 > $O/cap_a.raw
check "a 2x2 output works" cap_bytes $O/cap_a.raw 6
check "a zero size is refused" cap_refuses $A frames-out $CV/h264_270p.mp4 --size 0x0
check "a huge size is refused" cap_refuses $A frames-out $CV/h264_270p.mp4 --size 99999x99999
check "an unknown format is refused" cap_refuses $A frames-out $CV/h264_270p.mp4 --format yuv9
check "a sound-only file is refused" cap_refuses $A frames-out fixtures/audio/aac_stereo_44k.m4a
for f in $CV/h264_270p.mp4 $CV/hevc10_270p.mp4 $CP/rot90.mp4 $CP/vfr.mp4; do check "damaged $(basename $f) never crashes the frame reader" $C fuzz $f 24 3; done

if have_ffmpeg; then
    echo "== capture: frame reader against FFmpeg"
    cap_ff_same() { "$@" > $O/cap_b.raw 2> /dev/null; cmp -s $O/cap_a.raw $O/cap_b.raw; }
    Q=accurate_rnd+full_chroma_int+bicubic
    for size in 320x180 854x480 1280x720 97x61; do
        $A frames-out $CV/h264_270p.mp4 --size $size --format rgba --start 0.4 --count 3 > $O/cap_a.raw
        $F -ss 0.4 -i $CV/h264_270p.mp4 -frames:v 3 -vf scale=${size/x/:}:flags=$Q -pix_fmt rgba -f rawvideo $O/cap_b.raw
        check "rgba at $size is close to FFmpeg's bicubic scaling" $C psnr $O/cap_a.raw $O/cap_b.raw 45
        $A frames-out $O/hevc_720.mp4 --size $size --format i420 --start 1.3 --count 3 > $O/cap_a.raw
        $F -ss 1.3 -i $O/hevc_720.mp4 -frames:v 3 -vf scale=${size/x/:}:flags=$Q -pix_fmt yuv420p -f rawvideo $O/cap_b.raw
        check "i420 at $size from 720p is close to FFmpeg's bicubic scaling" $C psnr $O/cap_a.raw $O/cap_b.raw 45
    done
    for f in $CP/vfr.mp4 $CP/edits.mp4 $CP/delay.mp4 $CP/ntsc.mp4 $CV/h264_270p.mp4; do
        for at in 0 0.31 1.004; do
            case "$f $at" in *edits.mp4*1.004) continue ;; esac
            $A frames-out $f --format i420 --start $at --count 4 --no-rotate > $O/cap_a.raw
            check "$(basename $f) from $at s gives FFmpeg's -ss frames" cap_ff_same $F -ss $at -noautorotate -i $f -map 0:v:0 -frames:v 4 -fps_mode passthrough -pix_fmt yuv420p -f rawvideo -
        done
        for plan in "0.2 0.9 24" "0.05 0.9 50" "0.01 0.5 50" "0.21 0.4 24" "0.19 0.8 5" "0.123 0.7 29.97"; do
            set -- $plan
            $A frames-out $f --format i420 --start $1 --length $2 --fps $3 --no-rotate > $O/cap_a.raw
            check "$(basename $f) from $1 s for $2 s at a steady $3 fps equals FFmpeg's fps filter" cap_ff_same $F -ss $1 -noautorotate -i $f -map 0:v:0 -t $2 -vf fps=$3 -pix_fmt yuv420p -f rawvideo -
        done
    done
    for turn in 90 180 270; do
        $A frames-out $CP/rot$turn.mp4 --format i420 > $O/cap_a.raw
        check "rot$turn turns the way FFmpeg's autorotate does" cap_ff_same $F -i $CP/rot$turn.mp4 -pix_fmt yuv420p -f rawvideo -
    done
else
    skip "frame reader against FFmpeg's scaling, seeking, fps filter and autorotate"
fi

echo "== capture: frame writer, no FFmpeg needed"
for layout in rgba bgra rgb24; do
    $C pattern $O/cap_in.raw 640 360 30 $layout
    for codec in h264 hevc; do
        check "frames-in writes $layout as $codec" cap_quiet $A frames-in $O/cap_w.mp4 --size 640x360 --fps 30 --format $layout --codec $codec --in $O/cap_in.raw
        $A frames-out $O/cap_w.mp4 --format rgba > $O/cap_a.raw
        check "$layout through $codec comes back close to what went in" $C recolour $O/cap_in.raw $O/cap_a.raw $layout 40
        check "Apple's reader plays the $layout $codec file with 30 frames" eval "../build/avcheck $O/cap_w.mp4 | grep -q 'playable 1 duration 1.000 video_frames 30 '"
    done
done
$A frames-out $CV/h264_270p.mp4 --format i420 > $O/cap_in.raw
check "frames-in writes i420" cap_quiet $A frames-in $O/cap_w.mp4 --size 480x270 --fps 30 --format i420 --in $O/cap_in.raw
$A frames-out $O/cap_w.mp4 --format i420 > $O/cap_a.raw
check "i420 through H.264 at the default bitrate comes back above 42 dB" $C psnr $O/cap_in.raw $O/cap_a.raw 42
$C pattern $O/cap_in.raw 320 240 12 gray
check "frames-in writes gray mattes" cap_quiet $A frames-in $O/cap_w.mp4 --size 320x240 --fps 24 --format gray --in $O/cap_in.raw
$A frames-out $O/cap_w.mp4 --format i420 > $O/cap_a.raw
check "a gray matte comes back as the same luma with neutral colour" $C luma $O/cap_in.raw $O/cap_a.raw 320 240 40
$C pattern $O/cap_in.raw 479 269 8 rgba
check "frames-in takes an odd size" cap_quiet $A frames-in $O/cap_w.mp4 --size 479x269 --fps 25 --in $O/cap_in.raw
check "an odd size is stored padded to even" cap_info $O/cap_w.mp4 "size 480x270 .* frames 8 "
$A frames-out $O/cap_w.mp4 --format rgba --size 480x270 > $O/cap_a.raw
$C pattern $O/cap_in.raw 480 270 8 rgba
check "the padded picture keeps the content" $C psnr $O/cap_in.raw $O/cap_a.raw 38
$C pattern $O/cap_in.raw 320 180 30 rgba
check "frames-in from a pipe with a start time" eval "$A frames-in $O/cap_w.mp4 --size 320x180 --fps 30 --start 1.25 < $O/cap_in.raw 2> /dev/null"
check "the start time holds the first frame and shifts the rest" cap_info $O/cap_w.mp4 "frames 30 rate .* length 2.216667"
check "after the held first frame the second one sits at the start time" [ "$($A frames-out $O/cap_w.mp4 --start 1.25 --count 1 | md5)" = "$($A frames-out $O/cap_w.mp4 --frame 1 --count 1 | md5)" ]
check "just after the start time comes the third frame" [ "$($A frames-out $O/cap_w.mp4 --start 1.2501 --count 1 | md5)" = "$($A frames-out $O/cap_w.mp4 --frame 2 --count 1 | md5)" ]
check "frames-in with 29.97 fps as a decimal" cap_quiet $A frames-in $O/cap_w.mp4 --size 320x180 --fps 29.970030 --in $O/cap_in.raw
check "29.97 fps is stored as 30000/1001" cap_info $O/cap_w.mp4 "frames 30 rate 29.970030 length 1.001000"
check "frames-in stops at the limit" cap_quiet $A frames-in $O/cap_w.mp4 --size 320x180 --fps 30 --limit 0.5 --in $O/cap_in.raw
check "the limit keeps 15 frames" cap_info $O/cap_w.mp4 "frames 15 "
check "frames-in to Matroska" cap_quiet $A frames-in $O/cap_w.mkv --size 320x180 --fps 30 --codec hevc --in $O/cap_in.raw
check "the Matroska file reads back with 30 frames" cap_info $O/cap_w.mkv "frames 30 "

check "frames-in copies AAC sound from another file" cap_quiet $A frames-in $O/cap_w.mp4 --size 320x180 --fps 30 --audio-from fixtures/audio/aac_stereo_44k.m4a --in $O/cap_in.raw
$A pcm fixtures/audio/aac_stereo_44k.m4a --rate 44100 --channels 2 --format s16le --frames 44100 > $O/cap_a.raw 2> /dev/null
$A pcm $O/cap_w.mp4 --rate 44100 --channels 2 --format s16le > $O/cap_b.raw 2> /dev/null
check "copied sound is the same samples, cut at the end of the picture" cmp -s $O/cap_a.raw $O/cap_b.raw
check "Apple's reader plays picture and copied sound" eval "../build/avcheck $O/cap_w.mp4 | grep -q 'video_frames 30 audio_frames 44100 '"
for source in mp3_stereo_44k.mp4 opus_stereo_48k.mp4 flac_16_stereo_44k.mp4 aac_mono_44k.m4a; do
    rate=44100; case $source in opus*) rate=48000 ;; esac
    channels=2; case $source in *mono*) channels=1 ;; esac
    check "frames-in encodes a window of $source to AAC" cap_quiet $A frames-in $O/cap_w.mp4 --size 320x180 --fps 30 --audio-from fixtures/audio/$source --audio-start 0.25 --audio-length 0.7 --in $O/cap_in.raw
    $A pcm fixtures/audio/$source --rate $rate --channels $channels --format s16le --start 0.25 --frames $((rate * 7 / 10)) > $O/cap_a.raw 2> /dev/null
    $A pcm $O/cap_w.mp4 --rate $rate --channels $channels --format s16le > $O/cap_b.raw 2> /dev/null
    check "the encoded window of $source is the right length and sounds like the source" $C sound-like $O/cap_a.raw $O/cap_b.raw 30
done
check "a source without sound still gives a file" cap_quiet $A frames-in $O/cap_w2.mp4 --size 320x180 --fps 30 --audio-from $O/cap_w.mkv --in $O/cap_in.raw
check "a missing sound source is an error and leaves no file" eval "cap_refuses $A frames-in $O/cap_none.mp4 --size 320x180 --fps 30 --audio-from $O/nothing.m4a --in $O/cap_in.raw && [ ! -e $O/cap_none.mp4 ]"
head -c 100000 $O/cap_in.raw > $O/cap_short.raw
check "a cut-off last frame is an error and leaves no file" eval "cap_refuses $A frames-in $O/cap_none.mp4 --size 320x180 --fps 30 --in $O/cap_short.raw && [ ! -e $O/cap_none.mp4 ]"
check "no frames at all is an error and leaves no file" eval "cap_refuses $A frames-in $O/cap_none.mp4 --size 320x180 --fps 30 --in /dev/null && [ ! -e $O/cap_none.mp4 ]"
check "a bad frame rate is refused" cap_refuses $A frames-in $O/cap_none.mp4 --size 320x180 --fps 0 --in $O/cap_in.raw
check "an absurd steady rate is refused" cap_refuses $A frames-out $CV/h264_270p.mp4 --fps 5000

if have_ffmpeg; then
    echo "== capture: frame writer against FFmpeg"
    cap_frames() { [ "$($FFPROBE -v error -select_streams v:0 -count_frames -show_entries stream=nb_read_frames -of csv=p=0 $1)" = "$2" ]; }
    $A frames-in $O/cap_w.mp4 --size 320x180 --fps 30 --start 1.25 --audio-from fixtures/audio/aac_stereo_44k.m4a --in $O/cap_in.raw 2> /dev/null
    check "FFmpeg reads the frames-in file without complaint" clean $O/cap_w.mp4
    check "FFmpeg counts 30 frames" cap_frames $O/cap_w.mp4 30
    check "FFmpeg sees the held first frame and the shifted second one" [ "$($FFPROBE -v error -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 $O/cap_w.mp4 | sort -n | head -3 | tr '\n' ' ')" = "0.000000 1.250000 1.283333 " ]
    cap_beats_mpeg4() {
        $F -f lavfi -i "$1" -frames:v 45 -pix_fmt yuv420p -f rawvideo $O/cap_src.yuv
        $F -f rawvideo -pix_fmt yuv420p -s 1280x720 -r 30 -i $O/cap_src.yuv -c:v mpeg4 -q:v 2 -pix_fmt yuv420p $O/cap_m4.mp4
        $F -i $O/cap_m4.mp4 -pix_fmt yuv420p -f rawvideo $O/cap_m4.yuv
        $A frames-in $O/cap_ours.mp4 --size 1280x720 --fps 30 --format i420 --codec $2 --in $O/cap_src.yuv 2> /dev/null
        $A frames-out $O/cap_ours.mp4 --format i420 > $O/cap_ours.yuv
        python3 - $O/cap_src.yuv $O/cap_m4.yuv $O/cap_ours.yuv $O/cap_m4.mp4 $O/cap_ours.mp4 <<'PY'
import os, sys
sys.path.insert(0, ".")
import capturecheck as c
source = c.raw(sys.argv[1])
theirs, ours = c.psnr_of(source, c.raw(sys.argv[2])), c.psnr_of(source, c.raw(sys.argv[3]))
print("      mpeg4 -q:v 2: %.2f dB in %d kB; ours at the default bitrate: %.2f dB in %d kB" % (theirs, os.path.getsize(sys.argv[4]) // 1000, ours, os.path.getsize(sys.argv[5]) // 1000))
sys.exit(0 if ours >= theirs else 1)
PY
        local result=$?
        rm -f $O/cap_src.yuv $O/cap_m4.yuv $O/cap_ours.yuv
        return $result
    }
    check "H.264 at the default bitrate is at least as good as mpeg4 -q:v 2 (test card)" cap_beats_mpeg4 "testsrc2=size=1280x720:rate=30" h264
    check "H.264 at the default bitrate is at least as good as mpeg4 -q:v 2 (fractal zoom)" cap_beats_mpeg4 "mandelbrot=size=1280x720:rate=30" h264
    check "H.264 at the default bitrate is at least as good as mpeg4 -q:v 2 (grainy bars)" cap_beats_mpeg4 "smptehdbars=size=1280x720:rate=30,noise=alls=25:allf=t+u,gblur=sigma=1.2" h264
    check "HEVC at the default bitrate is at least as good as mpeg4 -q:v 2 (fractal zoom)" cap_beats_mpeg4 "mandelbrot=size=1280x720:rate=30" hevc
    check "HEVC at the default bitrate is at least as good as mpeg4 -q:v 2 (grainy bars)" cap_beats_mpeg4 "smptehdbars=size=1280x720:rate=30,noise=alls=25:allf=t+u,gblur=sigma=1.2" hevc
else
    skip "frame writer against FFmpeg's reader and against mpeg4 -q:v 2"
fi

echo "== capture: speech, no FFmpeg needed"
$C tone $O/cap_tone.wav 44100 3 440
$C voiced $O/cap_voiced.wav 44100 3 140
for pair in "1 1" "0.5 1" "0.75 1" "0.9 1" "1.5 1" "2 1" "1 0.5" "1 0.8" "1 1.25" "1 2" "0.7 1.3" "1.6 0.75"; do
    set -- $pair
    $A speech $O/cap_tone.wav $O/cap_said.wav --rate 44100 --speed $1 --pitch $2
    check "tone at speed $1 pitch $2: exact length, right frequency, no clicks" $C speech-tone $O/cap_tone.wav $O/cap_said.wav $1 $2 440
    $A speech $O/cap_voiced.wav $O/cap_said.wav --rate 22050 --speed $1 --pitch $2
    check "voiced sound at speed $1 pitch $2 to 22050 Hz: exact length, right pitch" $C speech-voice $O/cap_voiced.wav $O/cap_said.wav $1 $2
done
check "speech reads AIFF as the system voice writes it" $A speech fixtures/readers/s16.aiff $O/cap_said.wav --rate 44100 --speed 0.8
check "speech writes 16-bit mono WAV at the asked rate" $C wav $O/cap_said.wav 44100
check "a speed outside 0.25 to 4 is refused" cap_refuses $A speech $O/cap_tone.wav $O/cap_none.wav --speed 9
check "a pitch outside 0.25 to 4 is refused" cap_refuses $A speech $O/cap_tone.wav $O/cap_none.wav --pitch 0.1
check "a missing voice file is an error and leaves no file" eval "cap_refuses $A speech $O/nothing.aiff $O/cap_none.wav && [ ! -e $O/cap_none.wav ]"
if command -v say > /dev/null && say -o $O/cap_say.aiff "The quick brown fox jumps over the lazy dog, and then it rests." 2> /dev/null && [ -s $O/cap_say.aiff ]; then
    $A speech $O/cap_say.aiff $O/cap_say.wav --rate 44100
    for pair in "0.75 1" "1 1.3" "0.6 0.8"; do
        set -- $pair
        $A speech $O/cap_say.aiff $O/cap_said.wav --rate 44100 --speed $1 --pitch $2
        check "the system voice at speed $1 pitch $2: exact length, right pitch" $C speech-voice $O/cap_say.wav $O/cap_said.wav $1 $2 0.07
    done
else
    skip "the system voice (say is not available here)"
fi
if have_ffmpeg; then
    $F -i $O/cap_tone.wav -af "aresample=44100,asetrate=$((44100 * 5 / 4)),aresample=44100,atempo=0.80000,atempo=0.75000" -ar 44100 -ac 1 -c:a pcm_s16le $O/cap_ff.wav
    $A speech $O/cap_tone.wav $O/cap_said.wav --rate 44100 --speed 0.75 --pitch 1.25
    check "FFmpeg's asetrate and atempo chain lands on the same tone (its length is not exact)" $C speech-tone $O/cap_tone.wav $O/cap_ff.wav 0.75 1.25 440 2000
    check "ours lands on the same tone with the exact length" $C speech-tone $O/cap_tone.wav $O/cap_said.wav 0.75 1.25 440
else
    skip "speech against FFmpeg's asetrate and atempo chain"
fi

echo "== capture: devices, test devices and, only where access is already given, real devices"
$A devices > $O/cap_devices.txt
check "devices lists access for camera and microphone" [ "$(grep -c '^access' $O/cap_devices.txt)" = 2 ]
check "every device line has a kind, an id and a name" eval "! grep -v '^access' $O/cap_devices.txt | awk -F'\t' 'NF != 3 || (\$1 != \"video\" && \$1 != \"audio\") || \$2 == \"\" || \$3 == \"\"' | grep -q ."
$A capture $O/cap_test.mp4 --video test --audio test --seconds 2.6 --preview 320x180 > $O/cap_preview.raw 2> $O/cap_test.txt
check "the test camera and tone record to MP4" grep -q "^captured" $O/cap_test.txt
check "Apple's reader plays the recording" eval "../build/avcheck $O/cap_test.mp4 | grep -q 'playable 1 '"
check "picture and sound are in sync (flash against beep) and equally long" $C sync $O/cap_test.mp4 55 1.9
check "the preview gives BGRA frames at about 15 fps" eval "n=\$((\$(wc -c < $O/cap_preview.raw) / (320 * 180 * 4))); [ \$((\$(wc -c < $O/cap_preview.raw) % (320 * 180 * 4))) = 0 ] && [ \$n -ge 28 ] && [ \$n -le 42 ]"
check "a preview alone needs no file" eval "[ \$($A capture --video test --seconds 0.5 --preview native --preview-fps 10 2> /dev/null | wc -c) -ge $((1280 * 720 * 4 * 4)) ]"
check "the test camera alone records without sound" eval "$A capture $O/cap_test.mp4 --video test --seconds 1 2> /dev/null && ../build/avcheck $O/cap_test.mp4 | grep -q 'audio_frames -1 '"
check "test and real devices cannot be mixed" cap_refuses $A capture $O/cap_none.mp4 --video test --audio default --seconds 1
check "an unknown camera id is refused without a file" eval "cap_refuses $A capture $O/cap_none.mp4 --video no-such-camera --seconds 1 && [ ! -e $O/cap_none.mp4 ]"
cap_camera=$(grep '^video' $O/cap_devices.txt | head -1 | cut -f2)
cap_loop=$(grep '^audio' $O/cap_devices.txt | grep -i blackhole | head -1 | cut -f2)
if ! grep -q "^access.video.authorized" $O/cap_devices.txt; then
    check "without camera access capture stops with an error and never asks" eval "cap_refuses $A capture $O/cap_none.mp4 --video default --seconds 1 && grep -q \"^access.video.$(grep '^access.video' $O/cap_devices.txt | cut -f3)\" <($A devices)"
    skip "recording from a real camera (access is not already given)"
elif [ -z "$cap_camera" ]; then
    skip "recording from a real camera (this Mac has none)"
else
    cap_sound=""
    if grep -q "^access.audio.authorized" $O/cap_devices.txt; then
        [ -n "$cap_loop" ] && cap_sound="--audio $cap_loop"
        [ -n "${CAPTURE_REAL_MICROPHONE:-}" ] && cap_sound="--audio default"
    fi
    check "the real camera records" eval "$A capture $O/cap_real.mp4 --video '$cap_camera' $cap_sound --seconds 2 --preview 320x180 > $O/cap_preview.raw 2> /dev/null"
    check "Apple's reader plays the camera recording" eval "../build/avcheck $O/cap_real.mp4 | grep -q 'playable 1 '"
    check "the camera preview gives whole BGRA frames" eval "[ \$(wc -c < $O/cap_preview.raw) -ge $((320 * 180 * 4 * 5)) ] && [ \$((\$(wc -c < $O/cap_preview.raw) % (320 * 180 * 4))) = 0 ]"
fi
if grep -q "^access.audio.authorized" $O/cap_devices.txt && [ -n "$cap_loop" ]; then
    check "sound alone records from a loopback device through AVFoundation" cap_quiet $A capture $O/cap_real.m4a --audio "$cap_loop" --seconds 1.5
    check "the loopback recording is about 1.5 s of 48 kHz sound" eval "n=\$(../build/avcheck $O/cap_real.m4a | sed 's/.*audio_frames \([0-9-]*\).*/\1/'); [ \$n -ge 60000 ] && [ \$n -le 80000 ]"
else
    skip "recording sound from a real device (needs microphone access already given and a loopback device, so no headset is switched on)"
fi
rm -f $O/cap_*
