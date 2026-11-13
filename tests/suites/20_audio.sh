echo "== audio in: fixtures decode without FFmpeg"
FA=fixtures/audio
P="python3 audio.py"
for f in $(cut -d' ' -f3 $FA/decoded.md5); do check "$f decodes to the checked checksum" $P checksum $FA/$f $FA/decoded.md5; done
while read f rate channels seconds least slack; do
    check "$f: rate, channels, length to the sample, no offset from its source signal" $P truth $FA/$f $rate $channels $seconds $least $slack
done <<TABLE
aac_mono_44k.m4a 44100 1 1.5 24 0
aac_stereo_44k.m4a 44100 2 1.5 27 0
aac_stereo_48k.m4a 48000 2 1.5 23 0
aac_51_48k.m4a 48000 6 1.5 21 0
aac_apple_stereo_44k.m4a 44100 2 1.5 30 0
he_aac_stereo_44k.m4a 44100 2 1.5 22 0
he_aac_v2_stereo_44k.m4a 44100 2 1.5 1 0
alac_16_stereo_44k.m4a 44100 2 1.5 200 0
alac_24_stereo_44k.m4a 44100 2 0.5 80 0
mp3_stereo_44k.mp4 44100 2 1.5 23 0
mp3_stereo_22k.mp4 22050 2 1.5 23 0
mp2_stereo_44k.mp4 44100 2 1.5 19 0
aac_stereo_44k.mov 44100 2 1.5 27 0
flac_16_stereo_44k.mp4 44100 2 1.5 200 0
flac_24_stereo_44k.mp4 44100 2 0.5 80 0
opus_stereo_48k.mp4 48000 2 1.5 30 0
opus_mono_48k.mp4 48000 1 1.5 5 1
opus_51_48k.mp4 48000 6 1.5 12 1
pcm_s16le.mov 44100 2 0.5 200 0
pcm_s16be.mov 44100 2 0.5 200 0
pcm_s24le.mov 44100 2 0.5 80 0
pcm_s24be.mov 44100 2 0.5 80 0
pcm_s32le.mov 44100 2 0.5 80 0
pcm_f32le.mov 44100 2 0.5 80 0
pcm_f32be.mov 44100 2 0.5 80 0
pcm_f64le.mov 44100 1 0.5 80 0
pcm_u8.mov 22050 1 0.5 35 0
pcm_s16le_4ch.mov 44100 4 0.25 200 0
pcm_s16le.mp4 44100 2 0.5 200 0
pcm_s24be.mp4 44100 2 0.5 80 0
pcm_f32le.mp4 44100 2 0.5 80 0
TABLE
audio_frames() { [ "$($A adecode $FA/$1 --frames 0 2>&1)" = "$2" ]; }
check "AAC without an edit list keeps its priming: 73024 frames" audio_frames aac_stereo_48k_noedit.mp4 "48000 Hz, 2 channels, 73024 frames"
check "fragmented AAC: 67174 frames" audio_frames aac_stereo_44k_fragmented.mp4 "44100 Hz, 2 channels, 67174 frames"
same_with_raw_config() { [ "$($A adecode $FA/$1 2> /dev/null | md5)" = "$($A adecode $FA/$1 --raw-config 2> /dev/null | md5)" ]; }
for f in aac_stereo_44k.m4a aac_51_48k.m4a he_aac_v2_stereo_44k.m4a; do check "$f decodes the same from a bare AudioSpecificConfig" same_with_raw_config $f; done

echo "== audio in: random access equals one straight read, to the bit"
for f in aac_stereo_44k.m4a aac_51_48k.m4a aac_stereo_44k_fragmented.mp4 he_aac_stereo_44k.m4a he_aac_v2_stereo_44k.m4a alac_16_stereo_44k.m4a mp3_stereo_44k.mp4 mp3_stereo_22k.mp4 mp2_stereo_44k.mp4 flac_24_stereo_44k.mp4 opus_stereo_48k.mp4 opus_mono_48k.mp4 opus_51_48k.mp4 pcm_s24le.mov pcm_s16le_4ch.mov pcm_f32le.mp4; do
    check "random reads $f" $P chunks adecode $FA/$f 30 3
done
if command -v afconvert > /dev/null; then
    $P signal 44100 2 50 $O/au_long.wav
    afconvert -f m4af -d aac -b 128000 $O/au_long.wav $O/au_long_aac.m4a
    afconvert -f m4af -d aach -b 64000 $O/au_long.wav $O/au_long_he.m4a
    afconvert -f m4af -d alac $O/au_long.wav $O/au_long_alac.m4a
    rm -f $O/au_long.wav
    for f in au_long_aac.m4a au_long_he.m4a au_long_alac.m4a; do
        check "50 s $f: length and no offset from its source signal" $P truth $O/$f 44100 2 50 30 0
        check "50 s $f: random reads across restart points" $P chunks adecode $O/$f 60 5
    done
    check "50 s AAC as 48 kHz mono: random reads" $P chunks pcm $O/au_long_aac.m4a 60 6 --rate 48000 --channels 1
fi

echo "== resampler and channel mixer"
for pair in "44100 48000" "48000 44100" "48000 96000" "96000 44100" "44100 22050" "8000 48000" "48000 16000" "44100 47999"; do
    check "resample $pair Hz: SNR over 100 dB below 0.45 x the lower rate" $P resample $pair 100
done
check "resample 44100 to 48000 Hz pushed one frame at a time" $P resample 44100 48000 100 1
for pair in "48000 44100" "96000 48000" "44100 8000"; do check "resample $pair Hz rejects what cannot be kept" $P alias $pair; done
for job in "44100 48000 2 2" "48000 44100 6 2" "44100 44100 2 1" "22050 48000 1 2" "48000 32001 2 2"; do
    check "streaming $job: any push size gives the same output" $P streaming $job
done
check "5.1 to stereo levels" $P mixing $FA/aac_51_48k.m4a 2
check "5.1 to mono levels" $P mixing $FA/aac_51_48k.m4a 1
check "Opus 5.1 to stereo levels" $P mixing $FA/opus_51_48k.mp4 2
check "stereo to mono levels" $P mixing $FA/aac_stereo_48k.m4a 1
check "mono to stereo levels" $P mixing $FA/aac_mono_44k.m4a 2

echo "== PcmReader: any rate and channel count from any time, the same in any chunking"
while read f rate channels; do
    check "pcm $f as $rate Hz $channels channels: random reads" $P chunks pcm $FA/$f 30 4 --rate $rate --channels $channels
done <<TABLE
aac_stereo_44k.m4a 48000 2
aac_stereo_44k.m4a 22050 1
aac_51_48k.m4a 44100 2
aac_mono_44k.m4a 44100 2
he_aac_stereo_44k.m4a 48000 2
flac_16_stereo_44k.mp4 48000 2
mp3_stereo_44k.mp4 32000 1
opus_stereo_48k.mp4 44100 2
pcm_s24le.mov 96000 2
pcm_u8.mov 47999 2
TABLE
pcm_same_as_adecode() { [ "$($A pcm $FA/$1 --rate $2 --channels $3 2> /dev/null | md5)" = "$($A adecode $FA/$1 2> /dev/null | md5)" ]; }
check "pcm at the native rate and channels is the decoder's own output" pcm_same_as_adecode aac_stereo_48k.m4a 48000 2
$P signal 44100 2 0.5 $O/au_whole.wav
pcm_whole_numbers() { [ "$($A pcm $FA/pcm_s16le.mov --rate 44100 --channels 2 --format s16le 2> /dev/null | md5)" = "$(tail -c 88200 $O/au_whole.wav | md5)" ]; }
check "pcm s16le of a 16-bit file gives the source numbers back" pcm_whole_numbers

echo "== broken audio files never crash"
rounds=20
[ -n "${QUICK:-}" ] && rounds=4
check "damaged copies of every fixture" $P fuzz $FA $rounds 11

have_ffmpeg || return 0
echo "== audio in: against ffmpeg's own decode"
while read f least tail late extra; do
    check "$f against ffmpeg" $P against $FA/$f $least $tail $late $extra
done <<TABLE
aac_mono_44k.m4a 45 0 0
aac_stereo_44k.m4a 60 0 0
aac_stereo_48k.m4a 60 0 0
aac_stereo_48k_noedit.mp4 60 0 0
aac_stereo_44k_fragmented.mp4 60 410 0
aac_51_48k.m4a 55 0 0
aac_apple_stereo_44k.m4a 110 0 0
he_aac_stereo_44k.m4a 100 0 962
he_aac_v2_stereo_44k.m4a 100 0 962
mp3_stereo_44k.mp4 75 0 0
mp3_stereo_22k.mp4 75 0 0
mp2_stereo_44k.mp4 75 0 0
aac_stereo_44k.mov 60 0 0
opus_stereo_48k.mp4 110 648 0
opus_stereo_48k.mp4 80 648 0 libopus
opus_mono_48k.mp4 80 168 0 libopus
opus_51_48k.mp4 80 648 0 libopus
TABLE
for f in $(cut -d' ' -f3 $FA/decoded.md5); do check "$f bit-exact against ffmpeg" $P against $FA/$f 200 0 0 exact; done

echo "== PcmReader against ffmpeg -ar R -ac C"
while read f rate channels kind least slack start; do
    check "$f as $rate Hz $channels channels $kind${start:+ from $start s}" $P reader $FA/$f $rate $channels $kind $least $slack $start
done <<TABLE
aac_stereo_44k.m4a 48000 2 f32le 70 0
aac_stereo_44k.m4a 48000 2 s16le 70 0
aac_stereo_48k.m4a 44100 2 f32le 70 0
aac_stereo_48k.m4a 44100 1 f32le 70 0
aac_stereo_48k.m4a 44100 1 s16le 70 0
aac_mono_44k.m4a 48000 2 f32le 45 0
aac_51_48k.m4a 48000 2 f32le 60 0
aac_51_48k.m4a 44100 2 s16le 60 0
aac_51_48k.m4a 48000 1 f32le 60 0
mp3_stereo_44k.mp4 48000 2 f32le 70 0
opus_stereo_48k.mp4 44100 2 f32le 90 600
pcm_s16le.mov 44100 2 s16le 200 0
pcm_s16le.mov 48000 2 f32le 90 0
pcm_s24le.mov 22050 2 f32le 90 0
flac_16_stereo_44k.mp4 48000 2 f32le 90 0 0.5
alac_16_stereo_44k.m4a 32000 2 f32le 85 0 1.0
pcm_s16le.mov 48000 1 s16le 80 0 0.25
TABLE

echo "== audio in: 50 s files made by ffmpeg"
$P signal 44100 2 50 $O/au_long.wav
[ -f $O/au_ff_aac.m4a ] || $F -i $O/au_long.wav -c:a aac -b:a 128k $O/au_ff_aac.m4a
[ -f $O/au_ff_mp3.mp4 ] || $F -i $O/au_long.wav -c:a libmp3lame -b:a 128k $O/au_ff_mp3.mp4
[ -f $O/au_ff_opus.mp4 ] || $F -i $O/au_long.wav -ar 48000 -c:a libopus -b:a 96k $O/au_ff_opus.mp4
[ -f $O/au_ff_flac.mp4 ] || $F -i $O/au_long.wav -c:a flac $O/au_ff_flac.mp4
rm -f $O/au_long.wav
check "50 s AAC against ffmpeg" $P against $O/au_ff_aac.m4a 50 0 0
check "50 s MP3 against ffmpeg" $P against $O/au_ff_mp3.mp4 75 0 0
check "50 s Opus against ffmpeg" $P against $O/au_ff_opus.mp4 110 648 0
check "50 s FLAC bit-exact against ffmpeg" $P against $O/au_ff_flac.mp4 200 0 0 exact
for f in au_ff_aac.m4a au_ff_mp3.mp4 au_ff_opus.mp4 au_ff_flac.mp4; do check "50 s $f: random reads across restart points" $P chunks adecode $O/$f 60 5; done
check "the video suite's 10 s AAC track against ffmpeg" $P against $O/h264_720.mp4 60 0 0
check "the video suite's 10 s AAC track as 44.1 kHz stereo" $P reader $O/h264_720.mp4 44100 2 f32le 60 0
