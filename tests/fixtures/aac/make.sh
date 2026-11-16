#!/bin/bash
set -eu
cd "$(dirname "$0")"
T=$(mktemp -d)
S="python3 ../../audio.py signal"
$S 44100 1 1.5 $T/m44.wav
$S 44100 2 1.5 $T/s44.wav
$S 48000 2 1.5 $T/s48.wav
$S 48000 6 1.5 $T/x48.wav
$S 32000 2 1.5 $T/s32.wav
$S 22050 2 1.5 $T/s22.wav
afconvert -f m4af -d aach -b 32000 $T/m44.wav he_mono_44k.m4a
afconvert -f m4af -d aach -b 64000 $T/s48.wav he_stereo_48k.m4a
afconvert -f m4af -d aach -b 40000 $T/s32.wav he_stereo_32k.m4a
afconvert -f m4af -d aach -b 24000 $T/s22.wav he_stereo_22k.m4a
afconvert -f m4af -d aach -b 160000 $T/x48.wav he_51_48k.m4a
afconvert -f m4af -d aacp -b 24000 $T/s48.wav he_v2_48k.m4a
afconvert -f m4af -d aacp -b 24000 $T/s22.wav he_v2_32k.m4a
afconvert -f adts -d aach -b 48000 $T/s44.wav he_stereo_44k.aac
afconvert -f adts -d aacp -b 32000 $T/s44.wav he_v2_44k.aac
afconvert -f adts -d aac -b 128000 $T/s44.wav lc_stereo_44k.aac
rm -rf "$T"
