echo "== sound: G.711, ADPCM, TTA, PCM kinds and sound in WAV, AIFF, CAF, AVI, Matroska and QuickTime against checked tables (no FFmpeg)"
SX=fixtures/sound
SX_READS=20
[ -n "${QUICK:-}" ] && SX_READS=6
for f in $SX/*.wav $SX/*.aiff $SX/*.caf $SX/*.avi $SX/*.mkv $SX/*.mov; do
    check "$(basename $f) decodes to the checked checksum" python3 audio.py checksum $f $SX/sound.md5
    check "random reads and chunk sizes of $(basename $f) equal the straight decode" python3 audio.py chunks adecode $f $SX_READS 3
done
while read f name format; do
    check "probe names $f $name, $format" grep -q "\"codec_name\":\"$name\".*\"sample_fmt\":\"$format\"" <($A probe --json $SX/$f)
done <<TABLE
pcm_alaw.mkv pcm_alaw s16
pcm_mulaw.caf pcm_mulaw s16
pcm_s8.mov pcm_s8 s8
pcm_s64le.wav pcm_s64le s64
adpcm_ima_wav.avi adpcm_ima_wav s16p
adpcm_ima_qt.aiff adpcm_ima_qt s16p
adpcm_ms.mov adpcm_ms s16
ms_6ch.wav adpcm_ms s16p
adpcm_yamaha.mkv adpcm_yamaha s16
adpcm_g722.wav g722 s16
adpcm_g726_40k.wav g726 s16
flac.caf flac s32
tta_s16.mkv tta s16
tta_u8_mono.mkv tta u8
tta_s32.mkv tta s32
TABLE
SG="${GAPS_SAMPLES:-${WINMEDIA_SAMPLES:+$WINMEDIA_SAMPLES/../gaps_samples}}"
if [ -n "$SG" ] && [ -d "$SG/dk" ]; then
    echo "== sound: IMA ADPCM DK3 and DK4 from GAPS_SAMPLES (not committed)"
    for f in $SG/dk/*.avi; do
        check "$(basename $f) decodes to the checked checksum" python3 audio.py checksum $f $SX/samples.md5
        check "random reads of $(basename $f) equal the straight decode" python3 audio.py chunks adecode $f $SX_READS 5
    done
else
    skip "IMA ADPCM DK3 and DK4 sample files (set GAPS_SAMPLES to the sample folder)"
fi
python3 $SX/make.py random $O >/dev/null
check "make.py writes the random-payload fixtures again byte for byte" bash -c "for f in $O/ima[235]_*.wav $O/ms_6ch.wav; do cmp -s \$f $SX/\$(basename \$f) || exit 1; done"
rm -f $O/ima[235]_*.wav $O/ms_6ch.wav

have_ffmpeg || { skip "sound against ffmpeg's decoder"; return 0; }

echo "== sound: every committed fixture and sample bit-exact against ffmpeg's decoder"
for f in $SX/*.wav $SX/*.aiff $SX/*.caf $SX/*.avi $SX/*.mkv $SX/*.mov; do check "$(basename $f) bit-exact against ffmpeg" python3 wma_check.py exact $f; done
[ -n "$SG" ] && [ -d "$SG/dk" ] && for f in $SG/dk/*.avi; do check "$(basename $f) bit-exact against ffmpeg" python3 wma_check.py exact $f; done

echo "== sound: the widened audit, every sample format and container ffmpeg writes these codecs to, bit-exact"
mkdir -p $O/snd
SX_SECONDS=0.6
python3 $SX/make.py encoded $O/snd $SX_SECONDS
while read codec rate channels containers; do
    [ $channels = 1 ] && sig="aevalsrc=0.4*sin(2*PI*(300+900*t)*t):s=$rate:d=$SX_SECONDS" || sig="aevalsrc=0.4*sin(2*PI*(300+900*t)*t)|0.3*sin(2*PI*(500+400*t)*t):s=$rate:d=$SX_SECONDS"
    for c in $containers; do $F -f lavfi -i "$sig" -c:a $codec $O/snd/$codec.$c; done
done <<TABLE
pcm_u8 22050 2 wav avi mkv mov
pcm_s16le 22050 2 wav avi mkv mov caf
pcm_s16be 22050 2 mov aiff caf mkv
pcm_s24le 22050 2 wav avi mkv mov caf
pcm_s24be 22050 2 mov aiff caf mkv
pcm_s32le 22050 2 wav avi mkv mov caf
pcm_s32be 22050 2 mov aiff caf mkv
pcm_f32le 22050 2 wav avi mkv mov caf
pcm_f32be 22050 2 mov aiff caf
pcm_f64le 22050 2 wav mkv mov caf
pcm_f64be 22050 2 mov aiff caf
TABLE
while read format channels; do
    [ $channels = 1 ] && sig="aevalsrc=0.4*sin(2*PI*(300+900*t)*t):s=44100:d=2.5" || sig="aevalsrc=0.4*sin(2*PI*(300+900*t)*t)|0.3*sin(2*PI*(500+400*t)*t):s=44100:d=2.5"
    $F -f lavfi -i "$sig" -c:a tta -sample_fmt $format $O/snd/tta_${format}_$channels.mkv
done <<TABLE
s16 2
u8 1
s32 2
TABLE
for f in $O/snd/*.*; do check "$(basename $f) bit-exact against ffmpeg" python3 wma_check.py exact $f; done
rm -rf $O/snd

echo "== sound: lossy codecs in the containers newly read (system decoders, SNR against ffmpeg)"
mkdir -p $O/snd
while read codec rate container least; do
    $F -f lavfi -i "aevalsrc=0.4*sin(2*PI*(300+900*t)*t)|0.3*sin(2*PI*(500+400*t)*t):s=$rate:d=1" -c:a $codec $O/snd/$codec.$container
    check "$codec in $container against ffmpeg (SNR floor $least dB)" python3 audio.py against $O/snd/$codec.$container $least 0 0
done <<TABLE
eac3 48000 avi 60
ac3 48000 wav 60
mp2 48000 wav 60
libmp3lame 44100 wav 60
aac 44100 avi 50
TABLE
$F -f lavfi -i "aevalsrc=0.4*sin(2*PI*(300+900*t)*t)|0.3*sin(2*PI*(500+400*t)*t):s=44100:d=1" -c:a wmav2 $O/snd/wmav2.wav
check "wmav2 in WAV against ffmpeg (SNR floor 125 dB)" python3 wma_check.py against $O/snd/wmav2.wav 125
rm -rf $O/snd

echo "== sound: speed against ffmpeg (no more than 1.5 times its time)"
mkdir -p $O/snd
SX_LONG=120
[ -n "${QUICK:-}" ] && SX_LONG=40
while read codec rate channels container; do
    [ $channels = 1 ] && sig="aevalsrc=0.4*sin(2*PI*(300+900*t)*t):s=$rate:d=$SX_LONG" || sig="aevalsrc=0.4*sin(2*PI*(300+900*t)*t)|0.3*sin(2*PI*(500+400*t)*t):s=$rate:d=$SX_LONG"
    $F -f lavfi -i "$sig" -c:a $codec $O/snd/long_$codec.$container
    check "$codec: decode time against ffmpeg's" python3 wma_check.py speed $O/snd/long_$codec.$container 1.5
done <<TABLE
pcm_alaw 44100 2 wav
adpcm_ima_wav 44100 2 wav
adpcm_ima_qt 44100 2 mov
adpcm_ms 44100 2 wav
adpcm_yamaha 44100 2 wav
adpcm_g722 16000 1 wav
adpcm_g726 8000 1 wav
tta 44100 2 mkv
TABLE
rm -rf $O/snd
