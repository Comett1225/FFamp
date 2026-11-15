echo "== tools: trim, extract audio, compress, reverse, GIF and merge without FFmpeg"
FX=fixtures/tools
T=$O/tools
mkdir -p $T
find $T -type f -delete
tool() { $A "$@" > $T/last.log 2>&1; }
opens() { $A probe "$1" > /dev/null 2>&1 && ../build/avcheck "$1" > /dev/null 2>&1; }
judged() { python3 tools_check.py "$@" > $T/check.log 2>&1 || { cat $T/check.log; return 1; }; }
refused() { ! "${@:2}" && [ ! -e "$1" ] && [ ! -e "$1.part" ]; }
no_crash() { "$@" > /dev/null 2>&1; [ $? -lt 126 ]; }

tool trim $FX/clip.mp4 $T/trim_mid.mp4 --start 1.3 --end 2.5
check "trim 1.3 to 2.5 s opens here and in AVFoundation" opens $T/trim_mid.mp4
check "trim 1.3 to 2.5 s: key frame start, packets byte-identical, lengths, sound in step" judged trim $FX/clip.mp4 $T/trim_mid.mp4 1.3 2.5
tool trim $FX/clip.mp4 $T/trim_head.mp4 --start 0 --end 1.0
check "trim 0 to 1 s" judged trim $FX/clip.mp4 $T/trim_head.mp4 0 1.0
tool trim $FX/clip.mp4 $T/trim_tail.mp4 --start 3.21 --end 9
check "trim 3.21 s to past the end" judged trim $FX/clip.mp4 $T/trim_tail.mp4 3.21 9
check "trim tail opens in AVFoundation" opens $T/trim_tail.mp4
tool trim fixtures/readers/h264_aac.mkv $T/trim.mkv --start 1.2 --end 1.9
check "trim a Matroska file into Matroska" judged trim fixtures/readers/h264_aac.mkv $T/trim.mkv 1.2 1.9
tool trim fixtures/readers/vp9_opus.webm $T/trim.webm --start 0.6 --end 0.9
check "trim a WebM file (VP9 + Opus) into WebM" judged trim fixtures/readers/vp9_opus.webm $T/trim.webm 0.6 0.9
tool trim fixtures/probe/h264_aac.ts $T/trim_ts.mp4 --start 1.1 --end 1.7
check "trim a transport stream into MP4 opens in AVFoundation" opens $T/trim_ts.mp4
tool trim fixtures/audio/aac_stereo_48k.m4a $T/trim_sound.m4a --start 0.5 --end 1.5
check "trim a sound-only file to exactly one second" [ "$($A probe $T/trim_sound.m4a | grep -c 'duration 1.000 ')" = 1 ]
check "trimmed sound-only file opens in AVFoundation" opens $T/trim_sound.m4a
check "trim past the end of the file is refused, nothing left behind" refused $T/trim_none.mp4 tool trim $FX/clip.mp4 $T/trim_none.mp4 --start 50 --end 60
tool trim $FX/clip.mp4 $T/trim_progress.mp4 --start 0.2 --end 3.9
check "trim reports progress lines" judged progress $T/last.log

tool extract-audio $FX/clip.mp4 $T/copy.m4a
check "extract audio by copy: every packet identical" judged same_packets $T/copy.m4a audio $FX/clip.mp4
check "extract audio by copy opens in AVFoundation" opens $T/copy.m4a
tool extract-audio $FX/clip.mp4 $T/sound.m4a --aac --bitrate 96000
check "extract audio to AAC 96 kbit/s: length, bitrate, sound" judged sound_kept $FX/clip.mp4 $T/sound.m4a 96000
check "extract audio to AAC opens in AVFoundation" opens $T/sound.m4a
tool extract-audio $FX/clip.mp4 $T/sound.mp3 --mp3 --bitrate 128000
check "extract audio to MP3 128 kbit/s: length, bitrate, sound" judged sound_kept $FX/clip.mp4 $T/sound.mp3 128000
check "extract audio to MP3 opens in AVFoundation" opens $T/sound.mp3
tool extract-audio fixtures/audio/aac_51_48k.m4a $T/six.mp3 --mp3 --bitrate 128000
check "extract 5.1 sound to stereo MP3 opens in AVFoundation" opens $T/six.mp3
tool extract-audio fixtures/readers/vp9_opus.webm $T/named
check "extract Opus by copy picks .opus and keeps every packet" judged same_packets $T/named.opus audio fixtures/readers/vp9_opus.webm
tool extract-audio fixtures/audio/mp3_stereo_44k.mp4 $T/frames.mp3
check "extract MP3 by copy into an .mp3 file keeps every packet" judged same_packets $T/frames.mp3 audio fixtures/audio/mp3_stereo_44k.mp4
check "copying Vorbis into M4A is refused with the right extension named" refused $T/no.m4a tool extract-audio fixtures/readers/vp8_vorbis.webm $T/no.m4a
check "the refusal names .mka" grep -q "\.mka" $T/last.log
check "a file without sound is refused" refused $T/no2.m4a tool extract-audio fixtures/probe/vfr.mp4 $T/no2.m4a --aac

tool compress $FX/clip.mp4 $T/small.mp4 --start 0.5 --end 2.5 --size 160x90 --fps 15 --bitrate 200000 --audio aac --audio-bitrate 64000
check "compress 0.5 to 2.5 s, 160x90, 15 fps, 200 kbit/s, AAC: size, frames, bitrate, sound, right pictures" judged compress $FX/clip.mp4 $T/small.mp4 160 90 15 0.5 2.5 200000 aac
check "compress reports progress lines" judged progress $T/last.log
check "compressed file opens in AVFoundation" opens $T/small.mp4
tool compress $FX/clip.mp4 $T/double.mp4 --size 320x180 --fps 60 --bitrate 400000
check "compress to 60 fps repeats frames, sound copied" judged compress $FX/clip.mp4 $T/double.mp4 320 180 60 0 0 400000 copy
tool compress $FX/clip.mp4 $T/hevc.mp4 --size 160x90 --fps 30 --bitrate 150000 --codec hevc --audio none
check "compress to HEVC without sound" judged compress $FX/clip.mp4 $T/hevc.mp4 160 90 30 0 0 150000 none
check "HEVC file opens in AVFoundation" opens $T/hevc.mp4
tool compress $FX/clip.mp4 $T/vp9.webm --end 2 --size 160x90 --fps 30 --bitrate 150000 --codec vp9 --audio none
check "compress to VP9 in WebM" judged compress $FX/clip.mp4 $T/vp9.webm 160 90 30 0 2 150000 none 0.6
tool compress $FX/clip.mp4 $T/av1.mp4 --end 2 --size 160x90 --fps 30 --bitrate 150000 --codec av1 --audio aac
check "compress to AV1 in MP4" judged compress $FX/clip.mp4 $T/av1.mp4 160 90 30 0 2 150000 aac 0.6
tool compress fixtures/video/hevc10_270p.mp4 $T/from10.mp4 --fit 240x240 --fps 30 --bitrate 300000
check "compress 10-bit HEVC to 8-bit H.264, fitted to 240x136" judged compress fixtures/video/hevc10_270p.mp4 $T/from10.mp4 240 136 30 0 0 300000 copy 0.5
check "10-bit to 8-bit file opens in AVFoundation" opens $T/from10.mp4
tool compress fixtures/probe/vfr.mp4 $T/steady.mp4 --size 160x90 --fps 30 --bitrate 200000
check "compress a variable frame rate clip to a constant 30 fps" judged compress fixtures/probe/vfr.mp4 $T/steady.mp4 160 90 30 0 0 200000 none 0.6
tool compress fixtures/probe/rot90.mp4 $T/turned.mp4 --size 80x44 --fps 30 --bitrate 100000
check "compress keeps the rotation" [ "$($A probe $T/turned.mp4 | grep -c 'rotation 270 ')" = 1 ]
tool compress $FX/clip.mp4 $T/late.mp4 --start 3.99 --end 3.995 --size 160x90 --fps 30 --bitrate 200000
check "compress a span with no frame in it shows the frame before it" [ "$($A probe $T/late.mp4 | grep -c 'frames 1 ')" = 1 ]
tool compress $FX/clip.mp4 $T/ntsc.mp4 --size 160x90 --fps 30000/1001 --bitrate 200000
check "compress to 30000/1001 fps" judged compress $FX/clip.mp4 $T/ntsc.mp4 160 90 29.97003 0 0 200000 copy
tool compress fixtures/probe/delay.mp4 $T/late_copy.mp4 --size 160x90 --fps 30 --bitrate 150000
check "compress keeps sound that starts late where it was (copied)" judged sync fixtures/probe/delay.mp4 $T/late_copy.mp4
tool compress fixtures/probe/delay.mp4 $T/late_aac.mp4 --size 160x90 --fps 30 --bitrate 150000 --audio aac
check "compress keeps sound that starts late where it was (AAC, silence in front)" judged quiet_until $T/late_aac.mp4 0.4787
check "compress of a file without video is refused" refused $T/novideo.mp4 tool compress fixtures/audio/aac_stereo_48k.m4a $T/novideo.mp4
check "compress to a container that cannot hold the sound is refused" refused $T/bad.webm tool compress $FX/clip.mp4 $T/bad.webm --codec vp9 --size 160x90

stopped() {
    local out=$T/$1 signal=$2
    "${@:3}" > $T/stop.log 2>&1 &
    local pid=$!
    for n in $(seq 400); do grep -q "progress 0.0[2-9]" $T/stop.log && break; sleep 0.01; done
    kill -$signal $pid
    wait $pid
    [ $? = 130 ] && [ ! -e $out ] && [ -z "$(ls $T | grep "^$1\.")" ] && grep -q cancelled $T/stop.log
}
check "compress stopped with SIGINT leaves no file" stopped stop1.mp4 INT $A compress $FX/clip.mp4 $T/stop1.mp4 --size 1920x1080 --fps 240 --bitrate 20000000
check "compress stopped with SIGTERM leaves no file" stopped stop2.mp4 TERM $A compress $FX/clip.mp4 $T/stop2.mp4 --size 1920x1080 --fps 240 --bitrate 20000000 --audio aac
check "reverse stopped with SIGINT leaves no file, also no scratch file" stopped stop3.mp4 INT $A reverse $FX/clip.mp4 $T/stop3.mp4 --memory 1

tool reverse $FX/clip.mp4 $T/back.mp4
check "reverse the whole clip: frame N is source frame (last - N), timing, sound backwards" judged reverse $FX/clip.mp4 $T/back.mp4
check "reversed file opens in AVFoundation" opens $T/back.mp4
check "reverse reports progress lines" judged progress $T/last.log
tool reverse $FX/clip.mp4 $T/back_part.mp4 --start 0.7 --end 3.1 --memory 1
check "reverse 0.7 to 3.1 s with a 1 MB frame budget (scratch file used)" judged reverse $FX/clip.mp4 $T/back_part.mp4 0.7 3.1
check "the scratch file was used and removed" [ "$(grep -c 'spilled [1-9]' $T/last.log)" = 1 -a ! -e $T/back_part.mp4.spill.mp4 ]
tool reverse fixtures/probe/vfr.mp4 $T/back_vfr.mp4
check "reverse a variable frame rate clip" judged reverse fixtures/probe/vfr.mp4 $T/back_vfr.mp4
tool reverse fixtures/video/hevc10_270p.mp4 $T/back10.mp4
check "reverse a 10-bit HEVC clip into 8-bit H.264 opens in AVFoundation" opens $T/back10.mp4
check "reversed 10-bit clip has every frame" [ "$($A probe $T/back10.mp4 | grep -c 'video avc1 480x270 rotation 0 frames 48 ')" = 1 ]
tool reverse fixtures/probe/rot90.mp4 $T/back_turned.mp4
check "reverse keeps the rotation" [ "$($A probe $T/back_turned.mp4 | grep -c 'rotation 270 ')" = 1 ]
tool reverse fixtures/readers/vp9_opus.webm $T/back_webm.mp4
check "reverse VP9 + Opus into H.264 + AAC" judged reverse fixtures/readers/vp9_opus.webm $T/back_webm.mp4 0 0 28

for name in opaque alpha woven partial; do
    check "GIF $name: ImageIO frames and delays equal the reference drawing" judged gif_frames $FX/$name.gif $name.gif $T/$name.bgra
    tool gif-video $FX/$name.gif $T/$name.mp4 $T/$name-mask.mp4
    check "GIF $name: colour and matte videos (size, frame times, premultiplied colour, alpha)" judged gif_video $name.gif $T/$name.mp4 $T/$name-mask.mp4
    check "GIF $name: colour video opens in AVFoundation" opens $T/$name.mp4
done
check "GIF alpha: matte video opens in AVFoundation" opens $T/alpha-mask.mp4
check "a still GIF is refused, nothing left behind" refused $T/still.mp4 tool gif-video $FX/still.gif $T/still.mp4 $T/still-mask.mp4
check "no matte left behind either" [ ! -e $T/still-mask.mp4 -a ! -e $T/still-mask.mp4.part ]
check "a file that is not a GIF is refused" refused $T/notgif.mp4 tool gif-video $FX/clip.mp4 $T/notgif.mp4

tool merge $FX/dash_video.mp4 $FX/dash_audio.m4a $T/merged.mp4
check "merge fragmented H.264 + AAC pieces into MP4: every packet kept" judged merge $FX/dash_video.mp4 $FX/dash_audio.m4a $T/merged.mp4
check "merged MP4 opens in AVFoundation" opens $T/merged.mp4
tool merge $FX/dash_video.webm $FX/dash_audio.webm $T/merged.webm
check "merge VP9 + Opus pieces into WebM: every packet kept" judged merge $FX/dash_video.webm $FX/dash_audio.webm $T/merged.webm
tool merge $FX/dash_video.webm $FX/dash_audio.webm $T/auto_open
check "merge without an extension picks .webm for VP9 + Opus" [ -s $T/auto_open.webm ]
tool merge $FX/dash_video.mp4 $FX/dash_audio.webm $T/auto_mixed
check "merge picks .mp4 for H.264 + Opus and keeps every packet" judged merge $FX/dash_video.mp4 $FX/dash_audio.webm $T/auto_mixed.mp4
tool merge $FX/dash_video.mp4 fixtures/readers/vp8_vorbis.webm $T/auto_other
check "merge picks .mkv for H.264 + Vorbis and keeps every packet" judged merge $FX/dash_video.mp4 fixtures/readers/vp8_vorbis.webm $T/auto_other.mkv
check "merge into a container that cannot hold the codecs is refused" refused $T/wrong.webm tool merge $FX/dash_video.mp4 $FX/dash_audio.m4a $T/wrong.webm
tool strip-audio $FX/clip.mp4 $T/silent.mp4
check "strip-audio keeps every video packet" judged same_packets $T/silent.mp4 video $FX/clip.mp4
check "strip-audio leaves no sound" [ "$($A probe $T/silent.mp4 | grep -c audio)" = 0 ]
tool rewrap fixtures/probe/h264_aac.ts $T/from_ts.mp4
check "rewrap a transport stream into MP4: every video packet kept" judged same_packets $T/from_ts.mp4 video fixtures/probe/h264_aac.ts
check "rewrap a transport stream into MP4: every sound packet kept" judged same_packets $T/from_ts.mp4 audio fixtures/probe/h264_aac.ts
check "rewrap keeps picture and sound in step and starts at zero" judged sync fixtures/probe/h264_aac.ts $T/from_ts.mp4
check "rewrapped file opens in AVFoundation" opens $T/from_ts.mp4
tool rewrap fixtures/probe/delay.mp4 $T/delay.mkv
check "rewrap a file with late sound into Matroska keeps the gap" judged sync fixtures/probe/delay.mp4 $T/delay.mkv
tool rewrap fixtures/probe/delay.mp4 $T/delay.mp4
check "rewrap a file with late sound into MP4 keeps the gap" judged sync fixtures/probe/delay.mp4 $T/delay.mp4

head -c 70000 $FX/clip.mp4 > $T/cut.mp4
head -c 30000 $FX/dash_video.mp4 > $T/cut_frag.mp4
head -c 40001 fixtures/probe/h264_aac.ts > $T/cut.ts
head -c 20000 $FX/dash_video.webm > $T/cut.webm
head -c 2000 $FX/opaque.gif > $T/cut.gif
python3 -c "
import sys
data = bytearray(open('$FX/alpha.gif', 'rb').read())
for at in range(40, len(data), 7): data[at] ^= 0x5A
open('$T/noise.gif', 'wb').write(data)
data = bytearray(open('$FX/clip.mp4', 'rb').read())
for at in range(len(data) - 9000, len(data), 5): data[at] ^= 0xA5
open('$T/noise.mp4', 'wb').write(data)"
damaged_inputs_never_crash() {
    local f
    for f in cut.mp4 cut_frag.mp4 cut.ts cut.webm noise.mp4 cut.gif noise.gif; do
        no_crash $A trim $T/$f $T/d1.mp4 --start 0.5 --end 1.5 || return 1
        no_crash $A compress $T/$f $T/d2.mp4 --size 160x90 --fps 30 --audio aac || return 1
        no_crash $A reverse $T/$f $T/d3.mp4 || return 1
        no_crash $A rewrap $T/$f $T/d4.mkv || return 1
        no_crash $A extract-audio $T/$f $T/d5.m4a --aac || return 1
        no_crash $A merge $T/$f $T/$f $T/d6.mkv || return 1
        no_crash $A gif-video $T/$f $T/d7.mp4 $T/d7-mask.mp4 || return 1
        no_crash $A strip-audio $T/$f $T/d8.mp4 || return 1
    done
}
check "cut-off and scrambled inputs give an error or a file, never a crash (56 runs)" damaged_inputs_never_crash

have_ffmpeg || { skip "tools against the app's FFmpeg command lines"; return 0; }
clean_timed() { [ -z "$("$FFPROBE" -v error "$1" 2>&1)" ] && [ -z "$("$FFMPEG" -nostdin -v error -i "$1" -fps_mode passthrough -f null - 2>&1)" ]; }
echo "== tools: against what the app's FFmpeg command lines produce"
for f in trim_mid.mp4 trim_tail.mp4 trim.mkv trim.webm trim_ts.mp4 copy.m4a sound.m4a sound.mp3 named.opus small.mp4 double.mp4 hevc.mp4 vp9.webm av1.mp4 from10.mp4 steady.mp4 back.mp4 back_part.mp4 back_vfr.mp4 \
    opaque.mp4 alpha.mp4 alpha-mask.mp4 woven.mp4 woven-mask.mp4 partial.mp4 partial-mask.mp4 ntsc.mp4 late_copy.mp4 late_aac.mp4 trim_sound.m4a merged.mp4 merged.webm auto_mixed.mp4 auto_other.mkv silent.mp4 from_ts.mp4 delay.mkv delay.mp4; do
    check "$f has no ffprobe or decode messages" clean_timed $T/$f
done
$F -ss 1.300000 -i $FX/clip.mp4 -t 1.200000 -map 0:v:0 -map "0:a:0?" -c copy -avoid_negative_ts make_zero -movflags +faststart $T/ff_trim.mp4
check "trim: same first key frame, our video packets are among FFmpeg's" judged subset $T/trim_mid.mp4 $T/ff_trim.mp4 video
check "trim: our sound packets are among FFmpeg's" judged subset $T/trim_mid.mp4 $T/ff_trim.mp4 audio
$F -i $FX/clip.mp4 -vn -map 0:a:0 -c:a copy -movflags +faststart $T/ff_copy.m4a
check "extract by copy: the same packets as FFmpeg" judged same_packets $T/copy.m4a audio $T/ff_copy.m4a
$F -i $FX/clip.mp4 -vn -map 0:a:0 -c:a $FF_AAC -b:a 96k -movflags +faststart $T/ff_sound.m4a
check "extract to AAC: length, bitrate and sound like FFmpeg's" judged sound_like $T/sound.m4a $T/ff_sound.m4a 96000
$F -i $FX/clip.mp4 -vn -map 0:a:0 -c:a libmp3lame -b:a 128k $T/ff_sound.mp3
check "extract to MP3: length, bitrate and sound like FFmpeg's" judged sound_like $T/sound.mp3 $T/ff_sound.mp3 128000
$F -ss 0.500000 -noautorotate -i $FX/clip.mp4 -t 2.000000 -map 0:v:0 -map "0:a:0?" -vf scale=160:90,fps=15 -c:v $FF_H264 -b:v 200000 -g 15 -pix_fmt yuv420p \
    -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv -c:a $FF_AAC -b:a 64k -movflags +faststart $T/ff_small.mp4
check "compress: size, frames, length and frame times like FFmpeg's" judged against $T/small.mp4 $T/ff_small.mp4 18
check "compress: picture quality against the source not below FFmpeg's" judged quality $FX/clip.mp4 $T/small.mp4 $T/ff_small.mp4 0.5 15
near_size() { local a=$(stat -f %z $1) b=$(stat -f %z $2); [ $((a * 100)) -ge $((b * 70)) ] && [ $((a * 100)) -le $((b * 130)) ]; }
check "compress: file size within 30 % of FFmpeg's" near_size $T/small.mp4 $T/ff_small.mp4
$F -noautorotate -i $FX/clip.mp4 -map 0:v:0 -map "0:a:0?" -vf scale=320:180,fps=60 -c:v $FF_H264 -b:v 400000 -g 60 -pix_fmt yuv420p -c:a copy -movflags +faststart $T/ff_double.mp4
check "compress to 60 fps: the same frames at the same times as FFmpeg's" judged against $T/double.mp4 $T/ff_double.mp4 36
check "compress to 60 fps: picture quality against the source not below FFmpeg's" judged quality $FX/clip.mp4 $T/double.mp4 $T/ff_double.mp4 0 60
mkdir -p $T/parts
for i in 1 0; do
    start=$(python3 -c "print('%.6f' % max(0, ($i * 60 - 0.5) / 30))")
    $F -noautorotate -ss $start -t 2.000000 -i $FX/clip.mp4 -map 0:v:0 -map "0:a:0?" -frames:v 60 -vf "reverse,setpts=PTS-STARTPTS,pad=ceil(iw/2)*2:ceil(ih/2)*2" -c:v mpeg4 -q:v 2 -pix_fmt yuv420p \
        -af "areverse,asetpts=PTS-STARTPTS" -c:a pcm_s16le $T/parts/$i.mkv
done
printf "file '1.mkv'\nfile '0.mkv'\n" > $T/parts/list.txt
$F -f concat -safe 0 -i $T/parts/list.txt -c:v copy -c:a aac $T/ff_back.mp4
check "reverse: frames, length and pictures like the app's FFmpeg steps" judged against $T/back.mp4 $T/ff_back.mp4 36 ffyuv
for name in opaque alpha; do
    size=$($A gif-info $FX/$name.gif | head -1 | cut -d' ' -f4 | tr x :)
    $F -ignore_loop 1 -min_delay 2 -default_delay 10 -i $FX/$name.gif -vf "scale=$size:flags=neighbor,format=rgba,premultiply=inplace=1,format=yuv420p" -fps_mode vfr -c:v libx264 -pix_fmt yuv420p -an $T/ff_$name.mp4
    check "GIF $name: FFmpeg's colour video shows the same drawing (BT.601 luma)" judged gif_drawing $name.gif $T/ff_$name.mp4 601
    check "GIF $name: our colour video, decoded by FFmpeg, shows it too (BT.709 luma)" judged gif_drawing $name.gif $T/$name.mp4 709
done
check "GIF opaque: same size and frame count as FFmpeg's (it shows 0.01 s delays as they are, we use the app's 0.1 s rule)" judged against $T/opaque.mp4 $T/ff_opaque.mp4 0 none 0.25 0
check "GIF alpha: same size, frame count, length and frame times as FFmpeg's" judged against $T/alpha.mp4 $T/ff_alpha.mp4 0 none
$F -ignore_loop 1 -min_delay 2 -default_delay 10 -i $FX/alpha.gif -vf "scale=38:24:flags=neighbor,format=rgba,alphaextract,format=yuv420p" -fps_mode vfr -c:v libx264 -pix_fmt yuv420p -an $T/ff_alpha-mask.mp4
check "GIF alpha: matte video like FFmpeg's" judged against $T/alpha-mask.mp4 $T/ff_alpha-mask.mp4 30 rgb
$F -i $FX/dash_video.mp4 -i $FX/dash_audio.m4a -c copy -map 0:v:0 -map 1:a:0 -movflags +faststart $T/ff_merged.mp4
check "merge to MP4: the same video packets as FFmpeg's merge" judged same_packets $T/merged.mp4 video $T/ff_merged.mp4
check "merge to MP4: the same sound packets as FFmpeg's merge" judged same_packets $T/merged.mp4 audio $T/ff_merged.mp4
$F -i $FX/dash_video.webm -i $FX/dash_audio.webm -c copy -map 0:v:0 -map 1:a:0 $T/ff_merged.webm
check "merge to WebM: the same video packets as FFmpeg's merge" judged same_packets $T/merged.webm video $T/ff_merged.webm
check "merge to WebM: the same sound packets as FFmpeg's merge" judged same_packets $T/merged.webm audio $T/ff_merged.webm
$F -i $FX/clip.mp4 -map 0:v -c copy $T/ff_silent.mp4
check "strip-audio: the same packets as FFmpeg" judged same_packets $T/silent.mp4 video $T/ff_silent.mp4
$F -i fixtures/probe/h264_aac.ts -c copy -movflags +faststart $T/ff_from_ts.mp4
check "transport stream to MP4: the same sound packets as FFmpeg" judged same_packets $T/from_ts.mp4 audio $T/ff_from_ts.mp4
same_gap() {
    python3 -c "
import subprocess, sys
gaps = []
for path in sys.argv[1:3]:
    starts = [float(v) for v in subprocess.run(['$FFPROBE', '-v', 'error', '-show_entries', 'stream=start_time', '-of', 'csv=p=0', path], capture_output=True, text=True).stdout.split()]
    gaps.append(starts[0] - starts[1])
sys.exit(0 if abs(gaps[0] - gaps[1]) < 0.001 else 1)" $1 $2
}
check "transport stream to MP4: ffprobe sees the same gap between picture and sound as in FFmpeg's file (1 ms)" same_gap $T/from_ts.mp4 $T/ff_from_ts.mp4
