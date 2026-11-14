#!/bin/bash
set -eu
cd "$(dirname "$0")"
F="${FFMPEG:-ffmpeg} -nostdin -hide_banner -v error -y -fflags +bitexact"
S="testsrc2=rate=15"
N="noise=alls=12:allf=t"
$F -f lavfi -i "$S:size=100x76" -frames:v 10 -c:v msmpeg4v2 -g 5 -b:v 40k v2_odd.avi
$F -f lavfi -i "$S:size=128x96" -frames:v 6 -c:v msmpeg4 -q:v 2 v3_q2.avi
$F -f lavfi -i "$S:size=128x96,$N" -frames:v 8 -c:v msmpeg4 -b:v 64k v3_noise.mkv
$F -f lavfi -i "$S:size=128x96,$N" -frames:v 8 -c:v wmv1 -b:v 64k wmv1_noise.wmv
$F -f lavfi -i "$S:size=160x120" -frames:v 10 -c:v wmv2 -flags +loop -b:v 60k wmv2_loop.mkv
$F -f lavfi -i "$S:size=144x112" -frames:v 10 -c:v wmv2 -q:v 31 -g 5 wmv2_q31.avi
