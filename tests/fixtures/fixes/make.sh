#!/bin/bash
set -eu
cd "$(dirname "$0")"
F="ffmpeg -nostdin -hide_banner -v error -y"
levels() { n=$1; e=""; for i in $(seq 1 $n); do e="$e|0.1*$i*sin(2*PI*440*t)"; done; echo "${e:1}"; }
$F -f lavfi -i "aevalsrc=$(levels 7):c=6.1:s=8000:d=0.25" -c:a alac alac_61.m4a
$F -f lavfi -i "aevalsrc=$(levels 7):c=6.1:s=44100:d=0.3" -c:a aac -b:a 96k aac_61.m4a
$F -f lavfi -i "aevalsrc=$(levels 8):c=7.1:s=44100:d=0.3" -c:a aac -b:a 96k aac_71.m4a
