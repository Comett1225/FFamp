#!/bin/bash
set -eu
cd "$(dirname "$0")"
F="ffmpeg -nostdin -hide_banner -v error -y -bitexact"
T=$(mktemp -d)
S="python3 ../../audio.py signal"
$S 48000 6 1.5 $T/x48.wav
$S 48000 2 1.5 $T/s48.wav
$S 44100 2 1.5 $T/s44.wav
$S 44100 1 1.5 $T/m44.wav
$S 48000 3 1.5 $T/t48.wav
$S 8000 1 1.5 $T/m8.wav
$S 16000 1 1.5 $T/m16.wav
$S 44100 2 0.5 $T/p44.wav
$F -i $T/x48.wav -c:a ac3 -b:a 448k ac3_51.ac3
$F -i $T/x48.wav -c:a eac3 -b:a 384k eac3_51.eac3
$F -i $T/s48.wav -c:a ac3 -b:a 192k ac3_stereo.mp4
$F -i $T/x48.wav -c:a eac3 -b:a 384k eac3_51.mp4
$F -i $T/x48.wav -c:a ac3 -b:a 448k ac3_51.mov
$F -i $T/m44.wav -c:a ac3 -b:a 96k ac3_mono.mka
$F -i $T/t48.wav -af "channelmap=channel_layout=2.1" -c:a ac3 -b:a 192k ac3_21.ac3
clang -O1 -w amr_encode.c -I/opt/homebrew/include -L/opt/homebrew/lib -lopencore-amrnb -lvo-amrwbenc -o $T/amr_encode
$T/amr_encode nb $T/m8.wav amr_nb.amr
$T/amr_encode wb $T/m16.wav amr_wb.amr
$F -i amr_nb.amr -c copy amr_nb.3gp
$F -i amr_wb.amr -c copy amr_wb.3gp
afconvert -f caff -d BEI16 $T/p44.wav caf_s16be.caf
afconvert -f caff -d LEI24 $T/p44.wav caf_s24le.caf
afconvert -f caff -d LEF32 $T/p44.wav caf_f32le.caf
afconvert -f caff -d aac -b 128000 $T/s44.wav caf_aac.caf
afconvert -f caff -d aach -b 48000 $T/s44.wav caf_he_aac.caf
afconvert -f caff -d alac $T/s44.wav caf_alac.caf
afconvert -f caff -d flac $T/s44.wav caf_flac.caf
afconvert -f caff -d opus -b 96000 $T/s48.wav caf_opus.caf
$F -i $T/x48.wav -c:a ac3 -b:a 448k caf_ac3.caf
$F -i $T/s44.wav -c:a libmp3lame -b:a 128k caf_mp3.caf
$F -i amr_nb.amr -c copy caf_amr.caf
V="-f lavfi -i testsrc2=size=160x120:rate=25:duration=0.8"
D="-f lavfi -i testsrc2=size=720x576:rate=25:duration=0.08"
N="-f lavfi -i testsrc2=size=720x480:rate=30000/1001:duration=0.067"
A="-f lavfi -i sine=frequency=440:sample_rate=44100:duration=0.8"
$F $V -c:v mpeg4 -q:v 4 -g 10 mpeg4.mp4
$F $V -c:v mpeg4 -q:v 4 -g 10 mpeg4.mkv
$F $V -c:v mpeg4 -q:v 4 -g 10 -bf 1 mpeg4_asp.mp4
$F $V -c:v mjpeg -q:v 4 -pix_fmt yuvj420p mjpeg.mov
$F $V -c:v mjpeg -q:v 4 -pix_fmt yuvj422p mjpeg422.mov
$F $V -c:v mjpeg -q:v 4 -pix_fmt yuvj444p mjpeg444.mkv
$F $V -c:v mpeg2video -q:v 4 -g 10 -bf 2 mpeg2.mp4
$F $V -c:v mpeg2video -q:v 4 -g 10 -bf 2 mpeg2.mov
$F $V -c:v mpeg2video -q:v 4 -g 10 -bf 2 mpeg2.mkv
$F $V -c:v mpeg1video -q:v 4 -g 10 -bf 2 mpeg1.mov
$F $V -c:v mpeg1video -q:v 4 -g 10 -bf 2 mpeg1.mkv
$F $D -c:v dvvideo -pix_fmt yuv420p dv_pal.mov
$F $N -c:v dvvideo -pix_fmt yuv411p dv_ntsc.mov
$F -f lavfi -i testsrc2=size=176x144:rate=25:duration=0.8 -c:v h263 -q:v 4 -g 10 h263.3gp
$F $V $A -c:v mpeg4 -q:v 4 -g 10 -c:a libmp3lame -b:a 128k avi_mpeg4_mp3.avi
$F $V $A -c:v mpeg4 -q:v 4 -g 10 -bf 1 -c:a ac3 -b:a 128k avi_asp_ac3.avi
$F $V -c:v mpeg4 -q:v 4 -g 10 -bf 1 $T/asp.avi
python3 craft.py packed $T/asp.avi avi_packed.avi
$F $V $A -c:v mjpeg -q:v 4 -pix_fmt yuvj420p -c:a pcm_s16le avi_mjpeg_pcm.avi
$F $V $A -c:v mjpeg -q:v 4 -pix_fmt yuvj422p -c:a pcm_u8 avi_mjpeg422_pcm.avi
$F $V $A -c:v libx264 -preset fast -bf 2 -g 10 -pix_fmt yuv420p -c:a libmp3lame -b:a 128k avi_h264_mp3.avi
$F $V $A -c:v mpeg2video -q:v 4 -g 10 -bf 2 -c:a mp2 -b:a 128k avi_mpeg2_mp2.avi
$F $V $A -c:v mpeg1video -q:v 4 -g 10 -bf 2 -c:a pcm_s24le avi_mpeg1_pcm.avi
$F $D -f lavfi -i sine=frequency=440:sample_rate=48000:duration=0.08 -c:v dvvideo -pix_fmt yuv420p -c:a pcm_s16le avi_dv_pcm.avi
rm -r $T
