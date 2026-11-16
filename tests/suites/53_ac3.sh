echo "== AC-3 and E-AC-3: FFamp's own decoder against ffmpeg's at 135 dB, to the sample, every layout, rate and container"
if on_mac; then skip "AC-3 and E-AC-3 at 135 dB against ffmpeg (the Mac decodes them with AudioToolbox, 50 to 77 dB)"; return 0; fi
have_ffmpeg || { skip "AC-3 and E-AC-3 against ffmpeg's decoder"; return 0; }
DX=$O/dolby
rm -rf $DX
mkdir -p $DX/fz
for f in ac3_51.ac3 eac3_51.eac3 ac3_stereo.mp4 eac3_51.mp4 ac3_51.mov ac3_mono.mka ac3_21.ac3 caf_ac3.caf avi_asp_ac3.avi; do
    flags=
    [ $f = ac3_21.ac3 ] && flags=remix
    check "$f against ffmpeg at 135 dB, same length, no offset" python3 audio.py against fixtures/formats/$f 135 0 0 $flags
done
DX_LAYOUTS="mono:1 stereo:2 3.0(back):3:remix 3.0:3 quad(side):4:remix 4.0:4 5.0(side):5 FC+LFE:2:remix 2.1:3:remix FL+FR+LFE+BC:4:remix 3.1:4:remix 4.1:5:remix 5.1(side):6"
for rate in 32000 44100 48000; do
    for n in 1 2 3 4 5 6; do python3 audio.py signal $rate $n 1 $DX/s${n}_$rate.wav; done
done
for entry in $DX_LAYOUTS; do
    IFS=: read layout n flags <<< "$entry"
    for codec in ac3 eac3; do
        for rate in 32000 44100 48000; do
            low=64
            [ $n -ge 3 ] && low=128
            [ $n -ge 5 ] && low=192
            for kbps in $low 640; do
                $F -i $DX/s${n}_$rate.wav -af "channelmap=channel_layout=$layout" -c:a $codec -b:a ${kbps}k $DX/m.$codec
                check "$codec $layout $rate Hz $kbps kbit/s against ffmpeg" python3 audio.py against $DX/m.$codec 135 0 0 $flags
            done
        done
    done
done
for codec in ac3 eac3; do
    for entry in stereo:2 5.1:6 3.0\(back\):3:remix 2.1:3:remix; do
        IFS=: read layout n flags <<< "$entry"
        for container in mp4 mov mkv ts avi wav; do
            $F -i $DX/s${n}_48000.wav -af "channelmap=channel_layout=$layout" -c:a $codec -b:a 384k $DX/c.$container 2> /dev/null || { skip "$codec in $container (ffmpeg does not write it)"; continue; }
            check "$codec $layout in $container against ffmpeg" python3 audio.py against $DX/c.$container 135 0 0 $flags
            [ $layout = stereo ] && cp $DX/c.$container $DX/fz/${codec}_$layout.$container
        done
    done
    $F -i $DX/s6_44100.wav -c:a $codec -b:a 448k $DX/fz/${codec}_51.$codec
done
DX_LONG=120
[ -n "${QUICK:-}" ] && DX_LONG=40
for codec in ac3 eac3; do
    $F -f lavfi -i "aevalsrc=0.3*sin(2*PI*(200+400*t)*t)|0.3*sin(2*PI*(300+200*t)*t)|0.2*sin(2*PI*500*t)|0.3*sin(2*PI*60*t)|0.2*sin(2*PI*(700+50*t)*t)|0.2*sin(2*PI*(900+30*t)*t):s=48000:d=$DX_LONG:c=5.1(side)" -c:a $codec -b:a 448k $DX/long.$codec
    check "long 5.1 $codec against ffmpeg" python3 audio.py against $DX/long.$codec 135 0 0
    check "long 5.1 $codec: random reads and chunk sizes equal the straight decode" python3 audio.py chunks adecode $DX/long.$codec 40 7
    check "long 5.1 $codec: decode time against ffmpeg's (no more than 1.5 times)" python3 wma_check.py speed $DX/long.$codec 1.5
done
echo "== AC-3 and E-AC-3: syntax ffmpeg's encoders never write (dual mono, 2/2 with LFE, block switching, AHT, SPX, short frames, dependent substreams, enhanced coupling), written by ac3_make.py"
mkdir -p $DX/syn
python3 ac3_make.py $DX/syn 48 > $DX/syn.txt
while read f flags; do
    [ "$flags" = - ] && flags=
    if [ $f = ac3_half_rate.ac3 ]; then
        $F -i $DX/syn/$f -c copy $DX/syn/ac3_half_rate.mkv
        f=ac3_half_rate.mkv
    fi
    check "$f against ffmpeg" python3 audio.py against $DX/syn/$f 135 0 0 $flags
done < <(tr -d '\r' < $DX/syn.txt)
for f in ac3_51_switch.ac3 eac3_71.eac3 eac3_aht.eac3; do
    for i in $(seq 16); do cat $DX/syn/$f; done > $DX/syn/long_$f
    check "long_$f against ffmpeg" python3 audio.py against $DX/syn/long_$f 135 0 0
    check "long_$f: random reads and chunk sizes equal the straight decode" python3 audio.py chunks adecode $DX/syn/long_$f 30 9
done
cp $DX/syn/eac3_71.eac3 $DX/syn/eac3_spx.eac3 $DX/syn/ac3_dualmono.ac3 $DX/fz/
DX_ROUNDS=20
[ -n "${QUICK:-}" ] && DX_ROUNDS=5
check "damaged AC-3 and E-AC-3 files decode or fail cleanly" python3 audio.py fuzz $DX/fz $DX_ROUNDS 5 .ac3,.eac3,.mp4,.mkv,.ts,.avi
rm -rf $DX
