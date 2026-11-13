#!/bin/bash
set -eu
cd "$(dirname "$0")"
F="${FFMPEG:-ffmpeg} -nostdin -hide_banner -v error -y -fflags +bitexact"
V="-f lavfi -i testsrc2=size=160x120:rate=10:duration=1"
$F -f lavfi -i sine=frequency=440:sample_rate=22050:duration=1 -c:a wmav1 -b:a 32k wma1.wma
$F -f lavfi -i sine=frequency=330:sample_rate=44100:duration=1.5 -ac 2 -c:a wmav2 -b:a 64k -metadata title=Title -metadata artist=Artist \
    -metadata album=Album -metadata date=2004 -metadata comment=Comment -metadata genre=Genre -metadata track=3 wma2.wma
$F $V -f lavfi -i sine=frequency=440:sample_rate=22050:duration=1 -c:v wmv1 -b:v 100k -c:a wmav2 -b:a 32k wmv1_wma2.wmv
$F -f lavfi -i testsrc2=size=128x96:rate=10:duration=4 -c:v wmv2 -g 10 -b:v 60k wmv2.wmv
$F $V -f lavfi -i sine=frequency=440:sample_rate=22050:duration=1 -c:v msmpeg4 -b:v 100k -c:a libmp3lame -b:a 32k msmpeg4v3.asf
$F $V -c:v msmpeg4v2 -b:v 100k msmpeg4v2.asf
$F -f lavfi -i testsrc2=size=160x120:rate=25:duration=2 -f lavfi -i sine=frequency=440:sample_rate=48000:duration=2 -c:v libx264 -preset fast \
    -bf 2 -g 25 -pix_fmt yuv420p -c:a libmp3lame -b:a 48k h264_mp3.asf
$F $V -f lavfi -i sine=frequency=440:sample_rate=44100:duration=1 -c:v mpeg4 -q:v 4 -g 10 -c:a pcm_s16le mpeg4_pcm.asf
$F $V -c:v msmpeg4 -vtag DIV3 -b:v 100k avi_div3.avi
$F $V -c:v wmv2 -b:v 100k avi_wmv2.avi
ALL="wma1.wma wma2.wma wmv1_wma2.wmv wmv2.wmv msmpeg4v3.asf msmpeg4v2.asf h264_mp3.asf mpeg4_pcm.asf avi_div3.avi avi_wmv2.avi"
cd ../..
A=../build/ffamp
X=fixtures/asf
for f in $ALL; do echo "$($A packets $X/$f | md5)  $f"; done > $X/packets.md5
for f in $ALL; do echo "$($A probe --json $X/$f | md5)  $f"; done > $X/probe.md5
echo "$($A decode $X/h264_mp3.asf | md5)  h264_mp3.asf" > $X/pictures.md5
for f in h264_mp3.asf mpeg4_pcm.asf msmpeg4v3.asf; do echo "$($A adecode $X/$f 2> /dev/null | md5)  $f"; done > $X/sound.md5
