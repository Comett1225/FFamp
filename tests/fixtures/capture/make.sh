#!/bin/bash
set -eu
cd "$(dirname "$0")"
F="ffmpeg -nostdin -hide_banner -v error -y -bitexact"
Q=accurate_rnd+full_chroma_int+bicubic
$F -ss 0.666 -i ../video/h264_270p.mp4 -frames:v 1 -vf scale=240:136:flags=$Q -pix_fmt rgba -f rawvideo down_240x136.rgba
$F -ss 0.333 -i ../video/h264_270p.mp4 -frames:v 1 -vf scale=560:316:flags=$Q -pix_fmt yuv420p -f rawvideo up_560x316.i420
$F -i ../probe/rot90.mp4 -frames:v 1 -vf scale=120:214:flags=$Q -pix_fmt rgb24 -f rawvideo turned_120x214.rgb
$F -ss 0.2 -i ../video/hevc10_270p.mp4 -frames:v 1 -vf scale=320:180:flags=$Q -pix_fmt yuv420p -f rawvideo deep_320x180.i420
