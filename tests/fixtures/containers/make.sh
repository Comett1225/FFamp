#!/bin/bash
set -eu
cd "$(dirname "$0")"
F="ffmpeg -nostdin -hide_banner -v error -y"
T=$(mktemp -d)
$F -f lavfi -i testsrc2=size=160x120:rate=25 -t 2 -c:v libx264 -bf 2 -g 25 -pix_fmt yuv420p bframes.mp4
$F -i bframes.mp4 -c copy -bsf:v h264_mp4toannexb bframes_drops.avi
$F -i bframes.mp4 -c copy bframes_lengths.avi
$F -f lavfi -i testsrc2=size=160x120:rate=30 -t 6 -c:v libx264 -x264-params keyint=30:open-gop=1 -pix_fmt yuv420p -f avi open_gop.avi
$F -f lavfi -i testsrc2=size=160x120:rate=30 -t 2 -c:v libx265 -x265-params log-level=error -pix_fmt yuv420p hevc.mp4
$F -i hevc.mp4 -c copy -bsf:v hevc_mp4toannexb hevc_starts.avi
$F -i hevc.mp4 -c copy hevc_lengths.avi
$F -f lavfi -i testsrc2=size=160x120:rate=30 -t 1 -c:v libx264 -pix_fmt yuv420p $T/upright.mp4
$F -display_rotation 90 -i $T/upright.mp4 -c copy rot90.mp4
$F -i rot90.mp4 -c copy rot90.mkv
$F -display_rotation 270 -i $T/upright.mp4 -c copy $T/rot270.mp4
$F -i $T/rot270.mp4 -c copy rot270.mkv
$F -f lavfi -i testsrc2=size=160x120:rate=30 -t 1 -c:v libvpx-vp9 -b:v 100k $T/upright.webm
$F -display_rotation 90 -i $T/upright.webm -c copy rot90.webm
$F -f lavfi -i sine=f=440:r=44100:d=1 -c:a libmp3lame tone44.mp3
$F -f lavfi -i sine=f=660:r=48000:d=1 -c:a libmp3lame tone48.mp3
$F -f lavfi -i testsrc2=size=160x120:rate=30 -t 2 -c:v libx264 -bf 2 -g 30 -pix_fmt yuv420p $T/negative.mp4
$F -i $T/negative.mp4 -c copy -movflags frag_keyframe+empty_moov+negative_cts_offsets fragments_negative.mp4
rm -rf $T
