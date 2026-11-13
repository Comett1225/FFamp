#!/bin/bash
set -eu
cd "$(dirname "$0")"
F="ffmpeg -nostdin -hide_banner -v error -y"
V="testsrc2=size=320x180:rate=30"
SWEEP="aevalsrc=0.4*sin(2*PI*(300+150*t)*t)|0.4*sin(2*PI*(900-100*t)*t):sample_rate=48000"
$F -f lavfi -i $V -f lavfi -i "$SWEEP" -t 4 -c:v libx264 -preset slow -g 30 -bf 2 -b:v 220k -pix_fmt yuv420p -c:a aac -b:a 64k clip.mp4
$F -i clip.mp4 -an -c copy -t 2 -movflags frag_keyframe+empty_moov+default_base_moof dash_video.mp4
$F -i clip.mp4 -vn -c copy -t 2 -movflags frag_keyframe+empty_moov+default_base_moof dash_audio.m4a
$F -f lavfi -i $V -t 2 -c:v libvpx-vp9 -b:v 150k -g 30 -an dash_video.webm
$F -f lavfi -i "$SWEEP" -t 2 -c:a libopus -b:a 48k -vn dash_audio.webm
python3 gif_make.py
