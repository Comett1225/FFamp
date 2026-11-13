#!/bin/bash
set -eu
cd "$(dirname "$0")"
F="ffmpeg -nostdin -hide_banner -v error -y -bitexact"
T=$(mktemp -d)
S="python3 ../../audio.py signal"
$S 44100 1 1.5 $T/m44.wav
$S 44100 2 1.5 $T/s44.wav
$S 48000 2 1.5 $T/s48.wav
$S 48000 6 1.5 $T/x48.wav
$S 22050 2 1.5 $T/s22.wav
$S 48000 1 1.5 $T/m48.wav
$S 44100 2 0.5 $T/p16.wav
$S 44100 2 0.5 $T/p24.wav s24
$S 44100 2 0.5 $T/p32.wav s32
$S 44100 2 0.5 $T/pf32.wav f32
$S 44100 1 0.5 $T/pf64.wav f64
$S 22050 1 0.5 $T/p8.wav u8
$S 44100 4 0.25 $T/q16.wav
$F -i $T/m44.wav -c:a aac -b:a 96k aac_mono_44k.m4a
$F -i $T/s44.wav -c:a aac -b:a 128k aac_stereo_44k.m4a
$F -i $T/s48.wav -c:a aac -b:a 128k aac_stereo_48k.m4a
$F -i $T/s48.wav -c:a aac -b:a 128k -use_editlist 0 aac_stereo_48k_noedit.mp4
$F -i $T/s44.wav -c:a aac -b:a 128k -movflags frag_keyframe+empty_moov+default_base_moof -frag_duration 300000 aac_stereo_44k_fragmented.mp4
$F -i $T/x48.wav -c:a aac -b:a 256k aac_51_48k.m4a
afconvert -f m4af -d aac -b 128000 $T/s44.wav aac_apple_stereo_44k.m4a
afconvert -f m4af -d aach -b 48000 $T/s44.wav he_aac_stereo_44k.m4a
afconvert -f m4af -d aacp -b 32000 $T/s44.wav he_aac_v2_stereo_44k.m4a
$F -i $T/s44.wav -c:a alac alac_16_stereo_44k.m4a
$F -i $T/p24.wav -c:a alac alac_24_stereo_44k.m4a
$F -i $T/s44.wav -c:a libmp3lame -b:a 128k mp3_stereo_44k.mp4
$F -i $T/s22.wav -c:a libmp3lame -b:a 64k mp3_stereo_22k.mp4
$F -i $T/s44.wav -c:a mp2 -b:a 192k mp2_stereo_44k.mp4
$F -i $T/s44.wav -c:a aac -b:a 128k aac_stereo_44k.mov
$F -i $T/s44.wav -c:a flac flac_16_stereo_44k.mp4
$F -i $T/p24.wav -c:a flac flac_24_stereo_44k.mp4
$F -i $T/s48.wav -c:a libopus -b:a 96k opus_stereo_48k.mp4
$F -i $T/m48.wav -c:a libopus -b:a 48k -frame_duration 10 opus_mono_48k.mp4
$F -i $T/x48.wav -c:a libopus -b:a 192k opus_51_48k.mp4
$F -i $T/p16.wav -c:a pcm_s16le pcm_s16le.mov
$F -i $T/p16.wav -c:a pcm_s16be pcm_s16be.mov
$F -i $T/p24.wav -c:a pcm_s24le pcm_s24le.mov
$F -i $T/p24.wav -c:a pcm_s24be pcm_s24be.mov
$F -i $T/p32.wav -c:a pcm_s32le pcm_s32le.mov
$F -i $T/pf32.wav -c:a pcm_f32le pcm_f32le.mov
$F -i $T/pf32.wav -c:a pcm_f32be pcm_f32be.mov
$F -i $T/pf64.wav -c:a pcm_f64le pcm_f64le.mov
$F -i $T/p8.wav -c:a pcm_u8 pcm_u8.mov
$F -i $T/q16.wav -c:a pcm_s16le pcm_s16le_4ch.mov
$F -i $T/p16.wav -c:a pcm_s16le pcm_s16le.mp4
$F -i $T/p24.wav -c:a pcm_s24be pcm_s24be.mp4
$F -i $T/pf32.wav -c:a pcm_f32le pcm_f32le.mp4
clang -O1 -w vorbis_encode.c -I/opt/homebrew/include -L/opt/homebrew/lib -lvorbisenc -lvorbis -logg -o $T/vorbis_encode
$T/vorbis_encode $T/s44.wav 0.4 vorbis_stereo_44k.ogg
$T/vorbis_encode $T/x48.wav 0.3 vorbis_51_48k.ogg
$T/vorbis_encode $T/m44.wav 0.3 $T/m44.ogg
mkvmerge -q -o vorbis_mono_44k.mka $T/m44.ogg
mkvmerge -q -w -o vorbis_stereo_44k.webm vorbis_stereo_44k.ogg
rm -r $T
