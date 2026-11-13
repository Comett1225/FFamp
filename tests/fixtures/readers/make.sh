#!/bin/bash
set -eu
cd "$(dirname "$0")"
F="ffmpeg -nostdin -hide_banner -v error -y"
V=../video
TAGS=(-metadata title="Test Title" -metadata artist="Test Artist" -metadata album="Test Album")
tone() { echo "-f lavfi -i sine=frequency=440:sample_rate=$1:duration=$2"; }
mix() { echo "-f lavfi -i aevalsrc=0.4*sin(2*PI*440*t)+0.2*sin(2*PI*(300+900*t)*t)|0.4*sin(2*PI*660*t)+0.1*sin(2*PI*5000*t):s=$1:d=$2"; }

$F $(tone 8000 1) -c:a pcm_u8 u8.wav
$F $(mix 44100 1) "${TAGS[@]}" -c:a pcm_s16le s16.wav
$F $(tone 48000 0.5) -c:a pcm_s24le s24.wav
$F $(tone 22050 0.5) -c:a pcm_s32le s32.wav
$F $(tone 22050 0.5) -c:a pcm_f32le f32.wav
$F $(tone 22050 0.5) -c:a pcm_f64le f64.wav
$F $(tone 8000 0.5) -ac 6 -c:a pcm_s16le six.wav
$F $(tone 22050 0.5) -rf64 always -c:a pcm_s16le rf64.wav
$F $(mix 22050 1) "${TAGS[@]}" -write_id3v2 1 -c:a pcm_s16be s16.aiff
$F $(tone 8000 1) -c:a pcm_s8 s8.aiff
$F $(tone 22050 0.5) -c:a pcm_s24be s24.aiff
$F $(tone 22050 0.5) -c:a pcm_s16le -f aiff sowt.aifc
$F $(tone 22050 0.5) -c:a pcm_f32be -f aiff fl32.aifc
$F $(tone 22050 0.5) -c:a pcm_f64be -f aiff fl64.aifc

$F $(mix 44100 2) "${TAGS[@]}" -c:a libmp3lame -b:a 128k cbr.mp3
$F $(mix 22050 2) "${TAGS[@]}" -ac 1 -c:a libmp3lame -q:a 5 -id3v2_version 0 vbr.mp3
printf "TAG%-30s%-30s%-30s2026%-30s\377" "Old Title" "Old Artist" "Old Album" "" >> vbr.mp3
$F $(mix 48000 2) -c:a libmp3lame -b:a 96k -write_xing 0 -id3v2_version 0 plain.mp3
$F $(mix 48000 2) -c:a mp2 -b:a 128k layer2.mp2
( head -c 700 /dev/zero; cat plain.mp3 ) > padded.mp3
python3 craft.py vbri plain.mp3 vbri.mp3
for v in 3 4; do
    $F $(tone 22050 0.3) -c:a libmp3lame -b:a 32k -id3v2_version $v -metadata title="Тест Ünï 音 🎵" -metadata artist="Plain Artist" -metadata album="Älbum" -metadata date=2026 -metadata genre=Rock -metadata track=3 id3v2$v.mp3
done
$F $(mix 44100 2) -c:a aac -b:a 96k adts.aac
python3 craft.py adts_checksum adts.aac adts_checksum.aac

$F $(mix 44100 1) "${TAGS[@]}" -c:a flac fixed.flac
python3 craft.py flac_variable fixed.flac variable.flac
$F $(mix 48000 0.5) -ac 1 -sample_fmt s32 -bits_per_raw_sample 24 -c:a flac -frame_size 1024 deep.flac

$F $(mix 48000 2) "${TAGS[@]}" -c:a libopus -b:a 64k opus.ogg
$F $(mix 44100 2) "${TAGS[@]}" -c:a vorbis -strict -2 -b:a 96k vorbis.ogg
$F $(mix 44100 1) "${TAGS[@]}" -c:a flac -f ogg flac.ogg
$F $(tone 48000 1) -c:a libopus -b:a 32k second.ogg
cat opus.ogg second.ogg > chained.ogg
rm second.ogg
python3 craft.py ogg_small_pages opus.ogg opus_small_pages.ogg 1
python3 craft.py ogg_small_pages flac.ogg flac_small_pages.ogg 3

$F -i $V/h264_270p.mp4 -c copy "${TAGS[@]}" h264_aac.mkv
$F -i $V/hevc_270p.mp4 $(mix 48000 2) -map 0:v -map 1:a -c:v copy -c:a libopus -b:a 48k -shortest hevc_opus.mkv
SMALL="-f lavfi -i testsrc2=size=320x180:rate=30:duration=1"
$F $SMALL $(mix 48000 1) -c:v libvpx-vp9 -b:v 300k -g 15 -cpu-used 5 -c:a libopus -b:a 48k -shortest vp9_opus.webm
$F $SMALL $(mix 44100 1) -c:v libvpx -b:v 300k -g 15 -c:a vorbis -strict -2 -shortest vp8_vorbis.webm
$F $SMALL $(mix 48000 1) -c:v libsvtav1 -preset 10 -b:v 300k -g 15 -svtav1-params log=0 -c:a libopus -b:a 48k -shortest av1_opus.webm 2> /dev/null
$F -f lavfi -i testsrc2=size=128x72:rate=25:duration=0.2 $(tone 48000 0.2) -c:v prores_ks -profile:v 0 -c:a pcm_s16le -shortest prores_pcm.mkv
$F -f lavfi -i testsrc2=size=320x180:rate=30000/1001:duration=1 -c:v libx264 -preset fast -bf 3 -g 12 -pix_fmt yuv420p -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv bframes.mkv
$F $SMALL $(mix 48000 1) -c:v libvpx-vp9 -b:v 300k -g 15 -cpu-used 5 -c:a libopus -b:a 48k -shortest -f webm -live 1 - > live.webm
$F $(mix 44100 1) -c:a libmp3lame -b:a 96k mp3.mka
$F $(mix 44100 1) -c:a flac flac.mka
$F $(mix 48000 1) -c:a ac3 -b:a 96k ac3.mka
$F $(mix 48000 1) -c:a eac3 -b:a 96k eac3.mka
$F $(mix 44100 1) -c:a aac -b:a 96k aac.mka
$F $(mix 44100 0.5) -c:a alac alac.mka
$F $(tone 22050 0.5) -c:a pcm_s16be s16be.mka
$F $(tone 22050 0.5) -c:a pcm_s24le s24le.mka
$F $(tone 22050 0.5) -c:a pcm_f32le f32le.mka
$F $(mix 44100 1) -c:a libmp3lame -b:a 128k -write_xing 0 -id3v2_version 0 lace_source.mp3
python3 craft.py laced_mkv lace_source.mp3 lace_xiph.mka xiph 0 44100
python3 craft.py laced_mkv lace_source.mp3 lace_ebml.mka ebml 0 44100
python3 craft.py laced_mkv plain.mp3 lace_fixed.mka fixed 0 48000
python3 craft.py laced_mkv lace_source.mp3 stripped.mka xiph 2 44100
rm lace_source.mp3
