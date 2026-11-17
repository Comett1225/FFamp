echo "== audio fixes: sound read as the app reads it, against ffmpeg (system decoders on, as the app runs)"
have_ffmpeg || { skip "audio fixes against ffmpeg"; return 0; }
QX=$O/audiofix
rm -rf $QX
mkdir -p $QX
Q="python3 audiofix.py"
if on_mac; then QLEAST=20 QMIX=40; else QLEAST=100 QMIX=100; fi
$F -f lavfi -i "aevalsrc=0.3*sin(2*PI*440*t)|0.3*sin(2*PI*660*t):s=44100:d=3.3" -c:a pcm_s16le $QX/tone.wav
for rate in 8000 16000 22050 24000; do
    for kind in m4a aac; do
        $F -i $QX/tone.wav -c:a aac -ar $rate $QX/low$rate.$kind
        check "AAC LC at $rate Hz ($kind) decodes at its own rate, not silent, against ffmpeg" python3 audio.py against $QX/low$rate.$kind $QLEAST 0 0
    done
done
check "AAC LC at 22050 Hz as the app reads it at 48 kHz" $Q app $QX/low22050.m4a 48000 2 40
python3 audio.py signal 48000 6 1 $QX/n6.wav
python3 audio.py signal 48000 3 1 $QX/n3.wav
for entry in 2.1:n3 hexagonal:n6; do
    IFS=: read layout source <<< "$entry"
    $F -i $QX/$source.wav -af "channelmap=channel_layout=$layout" -c:a aac -b:a 320k $QX/pce.m4a
    if on_mac; then skip "aac $layout through a program config element (the Mac decodes AAC with AudioToolbox)"; else check "aac $layout through a program config element, in FFmpeg's order, against ffmpeg" python3 audio.py against $QX/pce.m4a 120 0 0; fi
done
if on_mac; then
    skip "implicit HE-AAC sample-aligned with ffmpeg (FFmpeg's and AudioToolbox's HE-AAC differ by 962 frames)"
else
    for f in he_stereo_44k he_v2_44k; do
        check "$f.aac (SBR not signalled) aligned to the sample with ffmpeg" python3 audio.py against fixtures/aac/$f.aac 120 0 0
        $F -i fixtures/aac/$f.aac -c copy $QX/$f.m4a
        $F -i fixtures/aac/$f.aac -c copy $QX/$f.mkv
        check "$f in MP4 aligned to the sample with ffmpeg" python3 audio.py against $QX/$f.m4a 120 0 0
        check "$f in Matroska aligned to the sample with ffmpeg" python3 audio.py against $QX/$f.mkv 120 0 0
    done
fi

$F -f lavfi -i "sine=f=440:r=48000:d=3.3" -ac 2 -c:a pcm_s16le $QX/s48.wav
for gap in 1 5; do
    $F -i $QX/s48.wav -af "asetpts='if(gte(T,1),PTS+$gap/TB,PTS)'" -c:a pcm_s16le $QX/gap${gap}_pcm.mkv
    $F -i $QX/s48.wav -af "asetpts='if(gte(T,1),PTS+$gap/TB,PTS)'" -c:a aac $QX/gap${gap}_aac.mp4
    check "a $gap s gap in PCM in Matroska stays silence, later sound at its time" $Q app $QX/gap${gap}_pcm.mkv 48000 2 100
    check "a $gap s gap in AAC in MP4 stays silence, later sound at its time" $Q app $QX/gap${gap}_aac.mp4 48000 2 30
done

QUAD="0.2*sin(2*PI*300*t)|0.2*sin(2*PI*500*t)|0.2*sin(2*PI*700*t)|0.2*sin(2*PI*1100*t)"
$F -f lavfi -i "aevalsrc=$QUAD:c=quad:s=48000:d=2" -c:a pcm_s16le $QX/quad.wav
$F -i $QX/quad.wav -c:a flac $QX/quad.flac
$F -i $QX/quad.wav -c:a pcm_s16le $QX/quad.mov
$F -i $QX/quad.wav -c:a pcm_s16le $QX/quad.avi
$F -i $QX/quad.wav -c:a flac $QX/quad_flac.mkv
$F -i $QX/quad.wav -c:a libopus -b:a 256k $QX/quad.opus 2> /dev/null
$F -i $QX/quad.wav -c:a libvorbis $QX/quad.ogg 2> /dev/null || $F -i $QX/quad.wav -c:a vorbis -strict -2 $QX/quad.ogg 2> /dev/null
$F -f lavfi -i "aevalsrc=0.2*sin(2*PI*300*t)|0.2*sin(2*PI*500*t)|0.2*sin(2*PI*700*t):c=2.1:s=48000:d=2" -c:a pcm_s16le $QX/c21.wav
$F -i $QX/c21.wav -c:a flac $QX/c21.flac
$F -i $QX/c21.wav -c:a flac $QX/c21_flac.mkv
$F -f lavfi -i "aevalsrc=$QUAD:c=3.1:s=48000:d=2" -c:a pcm_s16le $QX/c31.wav
for f in quad.wav quad.flac quad.mov quad.avi quad_flac.mkv c21.wav c21.flac c21_flac.mkv c31.wav; do
    check "$f mixed to stereo by its real layout, as ffmpeg mixes it" $Q app $QX/$f 48000 2 100
done
for f in quad.opus quad.ogg; do
    [ -f $QX/$f ] || { skip "$f (this ffmpeg cannot write it)"; continue; }
    check "$f mixed to stereo as quad, as ffmpeg mixes it" $Q app $QX/$f 48000 2 50
done

python3 audio.py signal 48000 6 3 $QX/c51.wav
$F -i $QX/c51.wav -c:a ac3 -b:a 448k $QX/ac3_c51.ac3
$F -i $QX/c51.wav -c:a ac3 -b:a 448k $QX/ac3_c51.mkv
$F -i $QX/c51.wav -c:a eac3 -b:a 448k $QX/eac3_c51.mp4
$F -i $QX/c51.wav -c:a ac3 -b:a 448k -surround_mixlev 0 $QX/ac3_mute.ac3
$F -i $QX/c51.wav -c:a ac3 -b:a 448k -dmix_mode ltrt -ltrt_cmixlev 0.5 -ltrt_surmixlev 0.5 $QX/ac3_ltrt.ac3
for f in ac3_c51.ac3 ac3_c51.mkv eac3_c51.mp4 ac3_mute.ac3 ac3_ltrt.ac3; do
    check "$f mixed to stereo with the stream's centre and surround levels" $Q app $QX/$f 48000 2 $QMIX
    check "$f mixed to mono with the stream's levels" $Q app $QX/$f 48000 1 $QMIX
done
if on_mac; then QS16=40; else QS16=70; fi
for f in ac3_c51.ac3 c51.wav; do
    check "$f played by the viewer (16-bit, downmix normalised as ffmpeg does)" $Q playback $QX/$f 48000 2 $QS16
done

python3 audio.py signal 44100 2 2.7 $QX/s27.wav
$F -i $QX/s27.wav -c:a flac $QX/s27.flac
for seed in 3 5 11; do
    $Q damage $QX/s27.flac $QX/bad$seed.flac $seed 300
    check "damaged FLAC (seed $seed) stays within full scale, same length" $Q bounded $QX/bad$seed.flac 119070
done

if $F -i $QX/s48.wav -c:a libopus -mapping_family 255 $QX/f255.ogg 2> /dev/null; then
    $F -i $QX/n3.wav -c:a libopus -mapping_family 255 $QX/f255c3.ogg
    check "Opus with channel mapping family 255 (stereo) decodes, as the app reads it" $Q app $QX/f255.ogg 48000 2 50
    check "Opus with channel mapping family 255 (3 channels) decodes, as the app reads it" $Q app $QX/f255c3.ogg 48000 2 50
else
    skip "Opus channel mapping family 255 (this ffmpeg has no libopus)"
fi

$F -f lavfi -i "aevalsrc=0.3+0.2*sin(2*PI*440*t)|0.3-0.2*sin(2*PI*660*t):s=48000:d=3.3" -c:a pcm_s16le $QX/dc48.wav
$F -f lavfi -i "aevalsrc=0.3+0.2*sin(2*PI*440*t)|0.3-0.2*sin(2*PI*660*t):s=44100:d=3.3" -c:a pcm_s16le $QX/dc44.wav
for rate in 48 44; do
    for container in webm mkv; do
        $F -i $QX/dc$rate.wav -c:a libvorbis -q:a 4 $QX/v$rate.$container 2> /dev/null || $F -i $QX/dc$rate.wav -c:a vorbis -strict -2 $QX/v$rate.$container
        check "Vorbis in $container at ${rate}.x kHz starts at its first sample, same length as ffmpeg" python3 audio.py against $QX/v$rate.$container 100 0 0
    done
done
rm -rf $QX
