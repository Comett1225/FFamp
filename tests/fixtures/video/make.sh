#!/bin/bash
set -eu
cd "$(dirname "$0")"
F="ffmpeg -nostdin -hide_banner -v error -y"
export SVT_LOG=1
S="testsrc2=size=480x270:rate=24"
N="-frames:v 48"
$F -f lavfi -i $S $N -c:v libx265 -preset medium -x265-params log-level=error:keyint=12 -b:v 500k -pix_fmt yuv420p10le -tag:v hvc1 hevc10_270p.mp4
$F -f lavfi -i $S $N -vf scale=out_color_matrix=bt2020:out_range=tv,format=yuv420p10le,setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc -c:v libx265 -preset medium -x265-params log-level=error:keyint=12:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc -b:v 500k -tag:v hvc1 hdr_pq_270p.mp4
$F -f lavfi -i $S $N -vf scale=out_color_matrix=bt2020:out_range=tv,format=yuv420p10le,setparams=color_primaries=bt2020:color_trc=arib-std-b67:colorspace=bt2020nc -c:v libx265 -preset medium -x265-params log-level=error:keyint=12:colorprim=bt2020:transfer=arib-std-b67:colormatrix=bt2020nc -b:v 500k -tag:v hvc1 hdr_hlg_270p.mp4
$F -f lavfi -i testsrc2=size=480x272:rate=25,eq=saturation=0.7 $N -c:v libx264 -preset medium -flags +ildct+ilme -x264opts tff=1:keyint=12 -b:v 500k -pix_fmt yuv420p h264_mbaff_272p.mp4
$F -f lavfi -i testsrc2=size=320x180:rate=24 -frames:v 12 -c:v prores_ks -profile:v 0 -pix_fmt yuv422p10le prores_180p.mov
$F -f lavfi -i $S $N -c:v libvpx-vp9 -b:v 500k -g 12 -pix_fmt yuv420p vp9_270p.mp4
$F -f lavfi -i $S $N -c:v libvpx-vp9 -b:v 500k -g 12 -pix_fmt yuv420p10le vp9_10bit_270p.mp4
$F -f lavfi -i $S $N -c:v libvpx -b:v 500k -g 12 -pix_fmt yuv420p vp8_270p.ivf
$F -f lavfi -i $S $N -c:v libsvtav1 -preset 6 -svtav1-params keyint=12 -b:v 500k -pix_fmt yuv420p av1_270p.mp4
$F -f lavfi -i $S $N -c:v libsvtav1 -preset 6 -svtav1-params keyint=12 -b:v 500k -pix_fmt yuv420p10le av1_10bit_270p.mp4
$F -i av1_270p.mp4 -c copy av1_270p.ivf
$F -i vp9_270p.mp4 -c copy vp9_270p.ivf
python3 make_paff.py paff.h264
$F -r 25 -i paff.h264 -c copy h264_paff_64p.mp4
rm paff.h264
