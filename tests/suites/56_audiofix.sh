echo "== audio fixes: sound read as the app reads it, against ffmpeg (system decoders on, as the app runs)"
FS=fixtures/audiofix
nf_frames() { [ "$($A adecode $FS/$1 --frames 0 2>&1)" = "$2" ]; }
nf_sum() { [ "$($A adecode $FS/$1 2> /dev/null | md5)" = "$2" ]; }
check "dts_51.mkv decodes without FFmpeg: 48000 Hz, 6 channels, 12288 frames" nf_frames dts_51.mkv "48000 Hz, 6 channels, 12288 frames"
check "dts_stereo.avi decodes without FFmpeg: 48000 Hz, 2 channels, 12288 frames" nf_frames dts_stereo.avi "48000 Hz, 2 channels, 12288 frames"
check "truehd_51.mkv decodes without FFmpeg to the checked checksum" nf_sum truehd_51.mkv a3049bb4fc42793e3a474f336fa36e73
check "wavpack_stereo.mkv decodes without FFmpeg to the checked checksum" nf_sum wavpack_stereo.mkv 5da1e8f47f1e605e403a0a677352c6b7
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
$F -i $QX/quad.wav -c:a libvorbis $QX/quad.ogg 2> /dev/null || rm -f $QX/quad.ogg
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
        $F -i $QX/dc$rate.wav -c:a libvorbis -q:a 4 $QX/v$rate.$container 2> /dev/null || { skip "Vorbis in $container (this ffmpeg has no libvorbis)"; continue; }
        check "Vorbis in $container at ${rate}.x kHz starts at its first sample, same length as ffmpeg" python3 audio.py against $QX/v$rate.$container 100 0 0
    done
done
python3 audio.py signal 48000 6 3.072 $QX/d6.wav
python3 audio.py signal 48000 2 3.072 $QX/d2.wav
$F -f lavfi -i testsrc2=size=320x240:rate=25 -t 3 -c:v libx264 -pix_fmt yuv420p $QX/picture.mp4 2> /dev/null || $F -f lavfi -i testsrc2=size=320x240:rate=25 -t 3 -c:v mpeg4 $QX/picture.mp4
for entry in dca:mkv:d6 dca:mp4:d2 dca:avi:d2 dca:wav:d2 dca:mkv:d2 truehd:mkv:d6 truehd:mkv:d2 truehd:mp4:d6 wavpack:mkv:d6 wavpack:mkv:d2 wavpack:mka:d6:fltp wavpack:mka:d6:s32p; do
    IFS=: read codec container source format <<< "$entry"
    out=$QX/${codec}_$source$format.$container
    $F -i $QX/$source.wav -c:a $codec ${format:+-sample_fmt $format} -strict -2 $out 2> /dev/null || { skip "$codec in $container (this ffmpeg cannot write it)"; continue; }
    if [ $codec = dca ]; then least=120; else least=250; fi
    check "$codec ($source) in $container decodes, same length and timing as ffmpeg" python3 audio.py against $out $least 0 0
    check "$codec ($source) in $container: random reads equal the straight decode" python3 audio.py chunks adecode $out 20 3
    mkdir -p $QX/fz && cp $out $QX/fz/
done
for entry in dca:mkv dca:avi dca:mp4 truehd:mkv; do
    IFS=: read codec container <<< "$entry"
    out=$QX/movie_$codec.$container
    $F -i $QX/picture.mp4 -i $QX/d6.wav -map 0:v -map 1:a -c:v copy -c:a $codec -strict -2 -shortest $out 2> /dev/null || { skip "film with $codec in $container (this ffmpeg cannot write it)"; continue; }
    if [ $codec = dca ]; then least=120; else least=250; fi
    check "film with $codec sound in $container: the sound decodes as ffmpeg decodes it" python3 audio.py against $out $least 0 0
    [ $container = avi ] || check "film with $codec sound in $container: mixed to stereo as the app reads it" $Q app $out 48000 2 100
done
QROUNDS=12
[ -n "${QUICK:-}" ] && QROUNDS=4
[ -d $QX/fz ] && check "damaged DTS, TrueHD and WavPack files decode or fail cleanly" python3 audio.py fuzz $QX/fz $QROUNDS 9 .mkv,.mka,.mp4,.avi,.wav
rm -rf $QX
