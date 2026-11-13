echo "== stand-ins: the two programs of experiments/absentmotion/shim accept the app's command lines and nothing else (no FFmpeg)"
cmake -S .. -B ../build -DABSENT_SHIM=ON > /dev/null && cmake --build ../build --target shim_ffmpeg shim_ffprobe > /dev/null || exit 1
SH=../build/shim
SO=$O/shim
rm -rf $SO
mkdir -p $SO
export ABSENT_SHIM_LOG=$SO/calls.log
: > $ABSENT_SHIM_LOG
refuses() { "$@" > $SO/refused.out 2> $SO/refused.err; [ $? = 64 ] && [ ! -s $SO/refused.out ] && grep -q 'not one it knows' $SO/refused.err; }
check "an unknown encoder command line is refused" refuses $SH/ffmpeg -i fixtures/probe/h264.mp4 -c copy $SO/never.mp4
check "an unknown probe command line is refused" refuses $SH/ffprobe -show_frames fixtures/probe/h264.mp4
check "a refused command writes no file" [ ! -e $SO/never.mp4 ]
check "a known command line with one word changed is refused" refuses $SH/ffprobe -v error -show_streams -show_format -of xml fixtures/probe/h264.mp4
for f in probe/h264.mp4 probe/hevc.mov readers/h264_aac.mkv audio/aac_stereo_44k.m4a; do
    check "stream and format description of $(basename $f) is the probe's own" [ "$($SH/ffprobe -v error -show_streams -show_format -of json fixtures/$f | md5)" = "$($A probe --json fixtures/$f | md5)" ]
done
check "encoder list names the hardware H.264 and AAC encoders" [ "$($SH/ffmpeg -hide_banner -encoders | awk 'length($1) == 6 { print $2 }' | tr '\n' ' ')" = "h264_videotoolbox aac_at " ]
PICTURE="-c:v h264_videotoolbox -realtime 0 -profile:v high -b:v 400000 -g 30 -pix_fmt yuv420p -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv"
check "encoder trial succeeds" $SH/ffmpeg -v error -f lavfi -i color=c=black:s=128x96:r=30:d=0.2 -vf scale=out_color_matrix=bt709:out_range=tv $PICTURE -frames:v 2 -f null -
python3 -c "
import sys, struct, math
frames = bytearray()
for n in range(45):
    frames += bytes((x * 2 + n * 4) & 255 if c == 0 else (y * 2) & 255 if c == 1 else 128 if c == 2 else 0 for y in range(96) for x in range(128) for c in range(4))
open(sys.argv[1], 'wb').write(frames)
open(sys.argv[2], 'wb').write(b''.join(struct.pack('<hh', int(12000 * math.sin(n * 0.0627)), int(12000 * math.sin(n * 0.0627))) for n in range(66150)))
" $SO/frames.rgb0 $SO/mix.pcm
export_movie() {
    $SH/ffmpeg -hide_banner -v error -f rawvideo -pix_fmt rgb0 -s 128x96 -r 30 -i - -f s16le -ar 44100 -ac 2 -i $SO/mix.pcm -map 0:v -vf scale=out_color_matrix=bt709:out_range=tv $PICTURE \
        -map 1:a -c:a aac_at -b:a 128k -ar 44100 -ac 2 -f mp4 -movflags +faststart -y $SO/movie.mp4 < $SO/frames.rgb0 2> $SO/movie.log
}
check "export from piped frames and a PCM file" export_movie
check "the exported movie has 45 frames" [ "$($A probe $SO/movie.mp4 | grep -c 'video avc1 128x96 rotation 0 frames 45 ')" = 1 ]
check "the exported movie has 1.5 s of 44.1 kHz stereo sound, to the sample" [ "$($A adecode $SO/movie.mp4 2>&1 > /dev/null | head -1)" = "44100 Hz, 2 channels, 66150 frames" ]
check "the exported movie opens in AVFoundation" ../build/avcheck $SO/movie.mp4
check "the exported movie's sound is the 440 Hz tone at the level that went in" python3 shim_check.py tone $SO/movie.mp4 440
check "first frame time of the export" [ "$($SH/ffprobe -v error -select_streams v:0 -read_intervals '%+#1' -show_entries frame=pts_time -of csv=p=0 $SO/movie.mp4)" = "0.000000" ]
check "packet times from 0.6 s start at the key frame at 0 s (45 lines)" [ "$($SH/ffprobe -v error -select_streams v:0 -read_intervals '0.6%' -show_entries packet=pts_time -of csv=p=0 $SO/movie.mp4 | wc -l | tr -d ' ')" = 45 ]
check "decoded frame times from 1.2 s start at the key frame at 1 s and end at the last frame" [ "$($SH/ffprobe -v error -select_streams v:0 -read_intervals '1.2%' -show_entries frame=pts_time -of csv=p=0 $SO/movie.mp4 | tr '\n' ' ' | cut -c1-18)$($SH/ffprobe -v error -select_streams v:0 -read_intervals '1.2%' -show_entries frame=pts_time -of csv=p=0 $SO/movie.mp4 | tail -1)" = "1.000000 1.033333 1.466667" ]
for f in "probe/h264.mp4 48000 2" "audio/aac_stereo_44k.m4a 44100 2" "audio/aac_51_48k.m4a 48000 6" "readers/opus.ogg 48000 2" "readers/cbr.mp3 44100 2"; do
    check "sound of ${f%% *} is the decoder's own, at the asked position" python3 shim_check.py sound fixtures/$f
done
for f in probe/h264.mp4 probe/hevc.mov readers/bframes.mkv probe/frag.mp4; do check "frame and packet times of $(basename $f) are in order" python3 shim_check.py times fixtures/$f; done
check "every call was written to the log" [ "$(grep -c 'called with' $ABSENT_SHIM_LOG)" -gt 30 ]
check "every refusal was written to the log" [ "$(grep -c REFUSED $ABSENT_SHIM_LOG)" = 3 ]
no_ffmpeg_linked() { ! otool -L $SH/ffmpeg $SH/ffprobe | grep -Eiq 'libav|libsw|ffmpeg[^:]*\.dylib'; }
check "the stand-ins link no FFmpeg library" no_ffmpeg_linked
unset ABSENT_SHIM_LOG

have_ffmpeg || { skip "stand-ins against the real programs"; return 0; }

echo "== stand-ins against the real programs"
for f in "probe/h264.mp4 48000 2" "probe/delay.mp4 48000 2" "audio/aac_stereo_44k.m4a 44100 2" "audio/aac_apple_stereo_44k.m4a 44100 2" "audio/aac_51_48k.m4a 48000 6" "audio/mp3_stereo_44k.mp4 44100 2" \
    "audio/aac_stereo_48k_noedit.mp4 48000 2" "readers/opus.ogg 48000 2" "readers/cbr.mp3 44100 2" "probe/h264_aac.ts 48000 2"; do
    check "sound of ${f%% *}: ffmpeg's length and over 40 dB (after a seek before the input, within 1 ms of ffmpeg's start)" python3 shim_check.py sound fixtures/$f judge
done
for f in probe/h264.mp4 probe/delay.mp4 probe/hevc.mov probe/vfr.mp4 probe/frag.mp4 probe/delay_frag.mp4 readers/h264_aac.mkv readers/bframes.mkv; do
    check "frame and packet times of $(basename $f) are ffprobe's" python3 shim_check.py times fixtures/$f judge
done
