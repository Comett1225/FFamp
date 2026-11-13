#!/bin/bash
set -eu
cd "$(dirname "$0")"
F="ffmpeg -nostdin -hide_banner -v error -y -bitexact"
T=$(mktemp -d)
X="${XVID_PREFIX:-/opt/homebrew}"
clang -O1 -w xvid_encode.c -I"$X/include" "$X/lib/libxvidcore.a" -o $T/xvid_encode
C="python3 ../formats/craft.py"
V="-f lavfi -i testsrc2=size=160x120:rate=25:duration=1.2"
I="-f lavfi -i testsrc2=size=176x144:rate=50,tinterlace=interleave_top -frames:v 30"
L="-c:v mpeg4 -q:v 4 -g 12 -bf 2"
M1=8,10,12,14,16,18,20,22,10,12,14,16,18,20,22,24,12,14,16,18,20,22,24,26,14,16,18,20,22,24,26,28,16,18,20,22,24,26,28,30,18,20,22,24,26,28,30,32,20,22,24,26,28,30,32,34,22,24,26,28,30,32,34,36
M2=16,17,18,19,20,21,22,23,17,18,19,20,21,22,23,24,18,19,20,21,22,23,24,25,19,20,21,22,23,24,25,26,20,21,22,23,24,25,26,27,21,22,23,24,25,26,27,28,22,23,24,25,26,27,28,29,23,24,25,26,27,28,29,30
$F $V $L -flags +qpel+mv4 -mbd 2 -trellis 1 lavc_qpel.mp4
$F -i lavc_qpel.mp4 -c copy lavc_qpel.mkv
$F $V $L -flags +qpel+mv4 -mbd 2 -trellis 1 lavc_qpel.avi
$C packed lavc_qpel.avi lavc_packed.avi
$F $I $L -flags +ildct+qpel -mbd 2 -alternate_scan 1 lavc_interlaced.mp4
$F $I -c:v mpeg4 -q:v 4 -g 12 -flags +ildct+ilme -alternate_scan 1 lavc_fields.mkv
$F $V $L -mpeg_quant 1 -intra_matrix $M1 -inter_matrix $M2 lavc_matrix.mkv
$F -f lavfi -i "testsrc2=size=320x240:rate=25,rotate=a=t*0.3:ow=160:oh=120" -frames:v 30 -pix_fmt yuv420p -f rawvideo - | $T/xvid_encode 160 120 25 30 $T/gmc.bin bf=2 qpel gmc packed
$C wrap $T/gmc.bin xvid_gmc.avi 160 120 25
$F $I -pix_fmt yuv420p -f rawvideo - | $T/xvid_encode 176 144 25 30 $T/fields.bin bf=2 interlaced matrix packed
$C wrap $T/fields.bin xvid_interlaced.avi 176 144 25
$F -i xvid_gmc.avi -c copy -f m4v xvid_gmc.m4v
rm -r $T
