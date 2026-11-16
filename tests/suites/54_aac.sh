echo "== AAC: FFamp's own decoder (LC, HE-AAC, HE-AAC v2) against ffmpeg's: LC at 135 dB, HE-AAC as close as ffmpeg's own C code is to its SIMD code"
if on_mac; then skip "AAC against ffmpeg at 135 dB (the Mac decodes AAC with AudioToolbox)"; return 0; fi
have_ffmpeg || { skip "AAC against ffmpeg's decoder"; return 0; }
export FFAMP_NO_SYSTEM_AUDIO=1
AX=$O/aacx
FAA=fixtures/aac
rm -rf $AX
mkdir -p $AX/fz
for f in $FAA/*.m4a $FAA/*.aac; do
    check "$(basename $f) against ffmpeg, same length, no offset" python3 audio.py against $f 135 0 0 peer
done
check "caf_he_aac.caf against ffmpeg (ffmpeg keeps the 2112 priming and 653 remainder frames the CAF trims)" python3 audio.py against fixtures/formats/caf_he_aac.caf 135 2765 2112 peer
for f in he_stereo_48k he_v2_48k he_51_48k he_mono_44k he_stereo_32k; do
    for container in mkv ts mov; do
        $F -i $FAA/$f.m4a -c copy $AX/$f.$container
        check "$f in $container against ffmpeg" python3 audio.py against $AX/$f.$container 135 0 0 peer
    done
done
cp $FAA/he_v2_44k.aac $FAA/he_51_48k.m4a $AX/he_stereo_48k.mkv $AX/he_v2_48k.ts $AX/fz/
AX_LAYOUTS="mono:1 stereo:2 2.1:3 3.0:3 3.0(back):3 quad:4 4.0:4 5.0:5 5.0(side):5 5.1:6 5.1(side):6 6.0:6 hexagonal:6 6.1:7 7.1:8 7.1(wide):8"
for n in 1 2 3 4 5 6 7 8; do python3 audio.py signal 48000 $n 1 $AX/n$n.wav; done
for entry in $AX_LAYOUTS; do
    IFS=: read layout n <<< "$entry"
    $F -i $AX/n$n.wav -af "channelmap=channel_layout=$layout" -c:a aac -b:a 320k $AX/l.m4a 2> /dev/null || { skip "aac $layout (ffmpeg does not encode it)"; continue; }
    check "aac $layout against ffmpeg at 135 dB" python3 audio.py against $AX/l.m4a 135 0 0
done
for rate in 8000 16000 22050 32000 44100 48000 96000; do
    python3 audio.py signal $rate 2 1 $AX/s$rate.wav
    for kbps in 24 96 256; do
        $F -i $AX/s$rate.wav -c:a aac -b:a ${kbps}k $AX/r.m4a
        check "aac stereo $rate Hz $kbps kbit/s against ffmpeg at 135 dB" python3 audio.py against $AX/r.m4a 135 0 0
    done
done
while read name flags; do
    $F -i $AX/s44100.wav -c:a aac $flags $AX/t.m4a
    check "aac $name against ffmpeg at 135 dB" python3 audio.py against $AX/t.m4a 135 0 0
done <<TABLE
noise_substitution_low_rate -b:a 32k -aac_pns 1
intensity_stereo -b:a 64k -aac_is 1 -aac_ms 0
mid_side_forced -b:a 96k -aac_ms 1
temporal_noise_shaping -b:a 128k -aac_tns 1
fast_coder -b:a 128k -aac_coder fast
twoloop_coder -b:a 160k -aac_coder twoloop
TABLE
python3 audio.py signal 48000 6 1 $AX/s6.wav
for container in mp4 mov mkv ts adts m4a; do
    out=$AX/c.$container
    [ $container = adts ] && out=$AX/c.aac
    $F -i $AX/s6.wav -c:a aac -b:a 320k $out 2> /dev/null || { skip "aac 5.1 in $container (ffmpeg does not write it)"; continue; }
    check "aac 5.1 in $container against ffmpeg at 135 dB" python3 audio.py against $out 135 0 0
    [ $container = mkv ] || [ $container = adts ] && cp $out $AX/fz/
done
AX_LONG=120
[ -n "${QUICK:-}" ] && AX_LONG=40
$F -f lavfi -i "aevalsrc=0.3*sin(2*PI*(200+400*t)*t)|0.3*sin(2*PI*(300+200*t)*t)|0.2*sin(2*PI*500*t)|0.3*sin(2*PI*60*t)|0.2*sin(2*PI*(700+50*t)*t)|0.2*sin(2*PI*(900+30*t)*t):s=48000:d=$AX_LONG:c=5.1" -c:a aac -b:a 384k $AX/long.m4a
check "long 5.1 aac against ffmpeg at 135 dB" python3 audio.py against $AX/long.m4a 135 0 0
check "long 5.1 aac: random reads and chunk sizes equal the straight decode" python3 audio.py chunks adecode $AX/long.m4a 40 7
check "long 5.1 aac: decode time against ffmpeg's (no more than 2 times: its MDCT is AVX2 assembly, ours plain C++)" python3 wma_check.py speed $AX/long.m4a 2
for f in he_stereo_44k he_v2_44k; do
    for i in $(seq $((AX_LONG / 8))); do cat $FAA/$f.aac; done > $AX/long_$f.aac
    check "long_$f.aac against ffmpeg" python3 audio.py against $AX/long_$f.aac 135 0 0 peer
    check "long_$f.aac: random reads and chunk sizes equal the straight decode (SBR and PS state carried across restart points)" python3 audio.py chunks adecode $AX/long_$f.aac 40 9
    check "long_$f.aac: decode time against ffmpeg's (no more than 1.5 times)" python3 wma_check.py speed $AX/long_$f.aac 1.5
done
AX_ROUNDS=20
[ -n "${QUICK:-}" ] && AX_ROUNDS=5
check "damaged AAC files decode or fail cleanly" python3 audio.py fuzz $AX/fz $AX_ROUNDS 5 .m4a,.aac,.mkv,.ts
rm -rf $AX
unset FFAMP_NO_SYSTEM_AUDIO
