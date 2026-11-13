#!/bin/bash
set -eu
cd "$(dirname "$0")"
F="ffmpeg -nostdin -hide_banner -v error -y"
V="testsrc2=size=320x180:rate=30"
S="testsrc2=size=160x90:rate=30"
TAGS=(-metadata title="My Title" -metadata artist="An Artist" -metadata album="The Album" -metadata date=2024 -metadata creation_time=2024-05-06T07:08:09Z)
$F -f lavfi -i $V -f lavfi -i sine=frequency=440:sample_rate=48000 -t 2 -c:v libx264 -g 30 -pix_fmt yuv420p -c:a aac -b:a 96k -ac 2 "${TAGS[@]}" h264.mp4
$F -f lavfi -i $V -f lavfi -i sine=frequency=440:sample_rate=44100 -t 2 -c:v libx265 -x265-params log-level=error -tag:v hvc1 -pix_fmt yuv420p -c:a aac -b:a 64k -ac 1 "${TAGS[@]}" -f mov hevc.mov
$F -i h264.mp4 -c copy -movflags frag_keyframe+empty_moov+default_base_moof "${TAGS[@]}" frag.mp4
for turn in 90 180 270; do
    $F -f lavfi -i $S -t 0.5 -c:v libx264 -pix_fmt yuv420p plain.mp4
    $F -display_rotation $turn -i plain.mp4 -c copy rot$turn.mp4
done
rm plain.mp4
$F -i h264.mp4 -c copy -metadata:s:a language=eng h264_aac.ts
$F -f lavfi -i $V -f lavfi -i sine=frequency=440:sample_rate=44100 -t 2 -c:v libx265 -x265-params log-level=error -pix_fmt yuv420p -c:a aac -b:a 96k -ac 2 hevc_aac.ts
$F -i h264.mp4 -c copy -f mpegts -mpegts_m2ts_mode 1 h264.m2ts
$F -f lavfi -i $V -f lavfi -i sine=frequency=440:sample_rate=44100 -t 3 -c:v libx264 -x264-params open-gop=1:keyint=30 -bf 3 -pix_fmt yuv420p -c:a mp2 -b:a 96k -ac 2 open_mp2.ts
$F -f lavfi -i $S -f lavfi -i sine=frequency=440:sample_rate=44100 -t 2 -c:v libx264 -pix_fmt yuv420p -c:a libmp3lame -b:a 64k -ac 1 h264_mp3.ts
$F -f lavfi -i $S -f lavfi -i sine=frequency=440:sample_rate=48000 -t 2 -c:v libx264 -pix_fmt yuv420p -c:a ac3 -b:a 96k -ac 2 h264_ac3.ts
$F -f lavfi -i $S -f lavfi -i sine=frequency=440:sample_rate=48000 -t 2 -c:v libx264 -pix_fmt yuv420p -c:a eac3 -b:a 96k -ac 2 h264_eac3.ts
$F -f lavfi -i $S -f lavfi -i sine=frequency=440:sample_rate=48000 -t 4 -c:v libx264 -pix_fmt yuv420p -c:a aac -b:a 64k -ac 2 -output_ts_offset 95441.5 wrap.ts
$F -f lavfi -i $S -itsoffset 0.5 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 2 -c:v libx264 -pix_fmt yuv420p -c:a aac -b:a 64k -ac 2 delay.mp4
$F -i delay.mp4 -c copy -movflags frag_keyframe+empty_moov+default_base_moof delay_frag.mp4
$F -f lavfi -i $S -t 3 -vf "select='not(eq(mod(n\,7)\,3))'" -fps_mode vfr -c:v libx264 -pix_fmt yuv420p vfr.mp4
$F -f lavfi -i $S -t 1 -c:v libx265 -x265-params log-level=error -tag:v hvc1 -pix_fmt yuv420p10le -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc -color_range tv hdr10.mp4
$F -f lavfi -i $S -t 1 -vf scale=out_range=pc -c:v libx264 -pix_fmt yuvj420p -color_range pc full.mp4
$F -f lavfi -i $S -t 1 -r 30000/1001 -c:v libx264 -profile:v baseline -pix_fmt yuv420p ntsc.mp4
$F -f lavfi -i sine=frequency=440:sample_rate=44100 -t 2 -c:a aac -b:a 64k -ac 1 "${TAGS[@]}" tags.m4a
$F -f lavfi -i $S -t 1 -c:v libx264 -pix_fmt yuv420p -color_primaries bt470bg -color_trc smpte170m -colorspace bt470bg inside.mp4
$F -i inside.mp4 -c copy -color_primaries bt709 -color_trc bt709 -colorspace bt709 colr.mp4
rm inside.mp4
python3 ../../probe_check.py edits h264.mp4 edits.mp4
ls -la
du -sh .
