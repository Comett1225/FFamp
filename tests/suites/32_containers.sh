echo "== containers: AVI, MP4, Matroska and MP3 findings of the review, read back by ffamp (no FFmpeg)"
CX=fixtures/containers
CO=$O/containers
CC="python3 containers_check.py"
rm -rf $CO
mkdir -p $CO
same_pictures() { [ "$($A decode "$1" "${@:3}" | md5)" = "$($A decode "$2" "${@:3}" | md5)" ]; }
probe_says() { $A probe "$1" | grep -q "$2"; }
json_says() { $A probe --json "$1" | grep -q "$2"; }
check "AVI H.264 with B-frames and drop chunks decodes in the MP4 file's order" same_pictures $CX/bframes_drops.avi $CX/bframes.mp4
check "AVI H.264 with B-frames and drop chunks decodes in the MP4 file's order (software)" same_pictures $CX/bframes_drops.avi $CX/bframes.mp4 --software
check "AVI drop chunks after the last picture are part of its length" json_says $CX/bframes_drops.avi '"duration":"2.000000"'
check "AVI H.264 with length-prefixed NAL units and avcC decodes to the MP4 file's frames" same_pictures $CX/bframes_lengths.avi $CX/bframes.mp4
check "AVI H.264 with length-prefixed NAL units keeps its avcC" probe_says $CX/bframes_lengths.avi "video avc1 160x120"
check "open-GOP AVI H.264 takes its key frames from the index" probe_says $CX/open_gop.avi "keyframes 6 "
if [ "$SYSTEM" = Windows ]; then
    skip "random frames and a backward walk of the open-GOP AVI through Media Foundation (it keeps its references across a jump to a key frame that is not an IDR picture)"
else
    check "random frames of the open-GOP AVI equal the straight decode" python3 comparex.py selfseek $CX/open_gop.avi 16 5
    check "every frame of the open-GOP AVI asked for from the last to the first equals the straight decode (a step back from a key frame to its leading picture)" python3 comparex.py selfback $CX/open_gop.avi
fi
on_mac || check "random frames of the open-GOP AVI equal the straight decode (software decoder)" python3 comparex.py selfseek $CX/open_gop.avi 16 5 --software
check "AVI HEVC with start codes decodes to the MP4 file's frames" same_pictures $CX/hevc_starts.avi $CX/hevc.mp4
check "AVI HEVC with length-prefixed NAL units and hvcC decodes to the MP4 file's frames" same_pictures $CX/hevc_lengths.avi $CX/hevc.mp4
check "AVI HEVC has the MP4 file's single key frame" probe_says $CX/hevc_starts.avi "video hvc1 160x120.*keyframes 1 "
check "Matroska Projection roll 90 is rotation 270, as the MP4 file" probe_says $CX/rot90.mkv "rotation 270"
check "Matroska Projection roll -90 is rotation 90" probe_says $CX/rot270.mkv "rotation 90"
check "WebM Projection roll 90 is rotation 270" probe_says $CX/rot90.webm "rotation 270"
check "Matroska rotation is in the probe JSON" json_says $CX/rot90.mkv '"rotation":90'
$A rewrap $CX/rot90.mp4 $CO/rot90.mkv > /dev/null 2>&1
check "the Matroska writer keeps the rotation of a rotated MP4" probe_says $CO/rot90.mkv "rotation 270"
$CC tagjoin $CX/tone44.mp3 $CX/tone44.mp3 $CO/tagged.mp3 1200000
cat $CX/tone44.mp3 $CX/tone48.mp3 > $CO/rates.mp3
pcm_seconds() { $A pcm "$1" --rate 44100 --channels 2 --format s16le 2> /dev/null > $CO/pcm.raw; python3 -c "import os, sys; sys.exit(abs(os.path.getsize('$CO/pcm.raw') / 176400 - $2) > 0.06)"; }
check "MP3 parts behind a 1.2 MB tag are read" pcm_seconds $CO/tagged.mp3 2.05
check "MP3 parts at another sample rate are read" pcm_seconds $CO/rates.mp3 2.05
rate_part() { $A pcm $CO/rates.mp3 --rate 44100 --channels 2 --format s16le 2> /dev/null > $CO/rates.raw && [ "$($CC pitch $CO/rates.raw 44100 "$1" "$2")" = "$3" ]; }
check "the 44.1 kHz part keeps its pitch" rate_part 0.2 0.8 440
check "the 48 kHz part keeps its pitch" rate_part 1.3 1.9 660
$CC edits $CX/bframes.mp4 $CO/two_edits.mp4 0.4:1024 0.4:16384
$CC edits $CX/bframes.mp4 $CO/zero_time.mp4 2:0
frames_of() { $A decode $CX/bframes.mp4 > $CO/all.yuv && $CC chosen $CO/all.yuv 28800 "$1" | md5; }
check "an MP4 with two edits plays both segments" [ "$($A decode $CO/two_edits.mp4 | md5)" = "$(frames_of 0-9,30-39)" ]
check "an MP4 with two edits has the length of both" probe_says $CO/two_edits.mp4 "frames 20 duration 0.800"
check "random frames of the two-edit MP4 equal the straight decode" python3 comparex.py selfseek $CO/two_edits.mp4 12 5
check "an MP4 edit at media time 0 starts the B-frame video at 0" json_says $CO/zero_time.mp4 '"start_time":"0.000000"'
check "an MP4 edit at media time 0 shows its first picture at 0" [ "$($A decode $CO/zero_time.mp4 --times 2> /dev/null | head -n 1)" = 0.000000 ]
check "fragments with negative composition offsets start the video one frame late, as FFmpeg" json_says $CX/fragments_negative.mp4 '"start_time":"0.033333"'
if have_ffmpeg; then
    ff_frames() { "$FFMPEG" -nostdin -v error -i "$1" -map 0:v -fps_mode passthrough -f rawvideo -pix_fmt yuv420p - | md5; }
    check "FFmpeg: AVI drop chunks decode to the same frames in the same order" [ "$(ff_frames $CX/bframes_drops.avi)" = "$($A decode $CX/bframes_drops.avi | md5)" ]
    check "FFmpeg: AVI with avcC decodes to the same frames" [ "$(ff_frames $CX/bframes_lengths.avi)" = "$($A decode $CX/bframes_lengths.avi | md5)" ]
    check "FFmpeg: the two-edit MP4 decodes to the same frames" [ "$(ff_frames $CO/two_edits.mp4)" = "$($A decode $CO/two_edits.mp4 | md5)" ]
    ff_keys() { "$FFPROBE" -v error -select_streams v -show_entries packet=flags -of csv=p=0 "$1" | grep -c K; }
    check "FFmpeg: the open-GOP AVI has the same key frames" probe_says $CX/open_gop.avi "keyframes $(ff_keys $CX/open_gop.avi) "
    ff_rotation() { [ "$("$FFPROBE" -v error -select_streams v -show_entries stream_side_data=rotation -of csv=p=0 "$1")" = "$2" ]; }
    check "FFmpeg: the rotation written by the Matroska writer is 90" ff_rotation $CO/rot90.mkv 90
    ff_start() { [ "$("$FFPROBE" -v error -select_streams v -show_entries stream=start_time -of csv=p=0 "$1")" = "$2" ]; }
    check "FFmpeg: the media time 0 edit starts at 0" ff_start $CO/zero_time.mp4 0.000000
    check "FFmpeg: the fragments start at 0.033333" ff_start $CX/fragments_negative.mp4 0.033333
    ff_seconds() { "$FFMPEG" -nostdin -v quiet -i "$1" -map 0:a -f s16le -ac 2 -ar 44100 - | wc -c | tr -d ' '; }
    close_length() { $A pcm "$1" --rate 44100 --channels 2 --format s16le 2> /dev/null > $CO/pcm.raw; python3 -c "import os, sys; sys.exit(abs(os.path.getsize('$CO/pcm.raw') - $(ff_seconds "$1")) > 2 * 1152 * 4)"; }
    check "FFmpeg: the MP3 behind a large tag has the same length within two frames" close_length $CO/tagged.mp3
    check "FFmpeg: the MP3 with two sample rates has the same length within two frames" close_length $CO/rates.mp3
else
    skip "containers: FFmpeg as the judge of the frame order, key frames, rotation, start times and MP3 lengths"
fi
rm -rf $CO
