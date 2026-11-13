#!/bin/bash
set -eu
cd "$(dirname "$0")"
F="${FFMPEG:-ffmpeg} -nostdin -hide_banner -v error -y -fflags +bitexact"
S="0.3*sin(2*PI*(220+800*t)*t)+0.1*(random(0)-0.5)|0.3*sin(2*PI*(330+600*t)*t)+0.1*(random(1)-0.5)"
M="0.3*sin(2*PI*(220+800*t)*t)+0.1*(random(0)-0.5)"
$F -f lavfi -i "aevalsrc=$M:s=11025:d=1" -c:a wmav1 -b:a 24k wma1_noise.wma
$F -f lavfi -i "aevalsrc=$S:s=44100:d=1" -c:a wmav2 -b:a 32k wma2_noise.wma
$F -f lavfi -i "aevalsrc=$M:s=16000:d=1" -c:a wmav2 -b:a 24k wma2_16k.wma
$F -f lavfi -i "aevalsrc=$S:s=32000:d=1" -c:a wmav1 -b:a 64k wma1_32k.mkv
$F -f lavfi -i "aevalsrc=$S:s=48000:d=1" -c:a wmav2 -b:a 64k wma2_48k.wma
$F -i wma2_48k.wma -c copy wma2_48k.avi
$F -i wma2_48k.wma -c copy wma2_48k.mkv
