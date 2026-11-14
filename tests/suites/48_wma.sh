echo "== wma: Windows Media Audio 1 and 2 against checked tables (no FFmpeg)"
WX=fixtures/wma
WC="python3 wma_check.py"
WX_ALL="$WX/wma1_noise.wma $WX/wma2_noise.wma $WX/wma2_16k.wma $WX/wma1_32k.mkv $WX/wma2_48k.wma $WX/wma2_48k.avi $WX/wma2_48k.mkv fixtures/asf/wma1.wma fixtures/asf/wma2.wma fixtures/asf/wmv1_wma2.wmv"
WX_READS=30
[ -n "${QUICK:-}" ] && WX_READS=10
for f in $WX_ALL; do
    check "$(basename $f) decodes to the checked checksum" python3 audio.py checksum $f $WX/sound.md5
    check "random reads and chunk sizes of $(basename $f) equal the straight decode" python3 audio.py chunks adecode $f $WX_READS 3
done
check "one stream in ASF and AVI decodes to the same samples" $WC same $WX/wma2_48k.wma $WX/wma2_48k.avi
check "and in Matroska" $WC same $WX/wma2_48k.wma $WX/wma2_48k.mkv
while read f name; do
    check "probe names $f $name" grep -q "\"codec_name\":\"$name\"" <($A probe --json $WX/$f)
done <<TABLE
wma2_48k.avi wmav2
wma2_48k.mkv wmav2
wma1_32k.mkv wmav1
TABLE
echo "== wma: Windows Media Audio 9 Professional against checked tables (no FFmpeg)"
PX=fixtures/wmapro
for f in $PX/pro_*; do
    check "$(basename $f) decodes to the checked checksum" python3 audio.py checksum $f $PX/sound.md5
    check "random reads and chunk sizes of $(basename $f) equal the straight decode" python3 audio.py chunks adecode $f $WX_READS 3
done
while read f name layout; do
    check "probe names $f $name, $layout" grep -q "\"codec_name\":\"$name\".*\"channel_layout\":\"$layout\"" <($A probe --json $PX/$f)
done <<TABLE
pro_71.wma wmapro 7.1
pro_51side.wma wmapro 5.1(side)
pro_quad.mkv wmapro quad
pro_5ch.avi wmapro 5.0
TABLE
check "7.1 mixed down to stereo by the mixer" python3 audio.py mixing $PX/pro_71.wma 2
check "5.1 mixed down to stereo and mono" bash -c "python3 audio.py mixing $PX/pro_51side.wma 2 && python3 audio.py mixing $PX/pro_51side.wma 1"
echo "== wma: Windows Media Audio 9 Lossless against checked tables (no FFmpeg)"
LX=fixtures/wmalossless
for f in $LX/lossless_*; do
    check "$(basename $f) decodes to the checked checksum" python3 audio.py checksum $f $LX/sound.md5
    check "random reads and chunk sizes of $(basename $f) equal the straight decode" python3 audio.py chunks adecode $f $WX_READS 3
done
check "probe names lossless_51.wma wmalossless, s16p, 5.1" grep -q '"codec_name":"wmalossless".*"sample_fmt":"s16p".*"channel_layout":"5.1"' <($A probe --json $LX/lossless_51.wma)
check "probe names lossless_stereo24.wma s32p" grep -q '"sample_fmt":"s32p"' <($A probe --json $LX/lossless_stereo24.wma)
rm -rf $O/wx_fz
mkdir -p $O/wx_fz/seeds $O/wx_fz/corpus
for f in $WX/*.wma fixtures/asf/wma*.wma $PX/*.wma $LX/*.wma; do $WC seed $f $O/wx_fz/seeds/$(basename $f).seed; done
check "fuzz_wma: the format headers and packets of the fixtures go through" env TMPDIR=$O/wx_fz ../build/fuzz_wma $O/wx_fz/seeds/*
check "fuzz_wma: ${FUZZ_BURST:-3} s of mutated headers and packets, no crash, hang or memory blow-up" env TMPDIR=$O/wx_fz ../build/fuzz_wma -seed=13 -max_total_time=${FUZZ_BURST:-3} -corpus_limit_mb=0 -rss_limit_mb=2048 -artifact_prefix=$O/wx_fz/ $O/wx_fz/corpus $O/wx_fz/seeds 2> $O/wx_fz/log.txt

have_ffmpeg || { rm -rf $O/wx_*; skip "wma against ffmpeg's decoder"; return 0; }

echo "== wma: every sample against ffmpeg's decoder (SNR floor 125 dB, lengths to the sample)"
WX_FLOOR=125
for f in $WX_ALL; do check "$(basename $f) against ffmpeg" $WC against $f $WX_FLOOR; done
WX_SECONDS=3
[ -n "${QUICK:-}" ] && WX_SECONDS=1.5
WX_STEREO="0.3*sin(2*PI*(220+900*t)*t)+0.1*(random(0)-0.5)|0.25*sin(2*PI*(330+700*t)*t)+0.1*(random(1)-0.5)"
WX_MONO="0.3*sin(2*PI*(220+900*t)*t)+0.1*(random(0)-0.5)"
while read codec rate channels rate_bits; do
    f=$O/wx_${codec}_${rate}_${channels}_${rate_bits}.wma
    [ $channels = 1 ] && signal=$WX_MONO || signal=$WX_STEREO
    $F -f lavfi -i "aevalsrc=$signal:s=$rate:d=$WX_SECONDS" -c:a $codec -b:a $rate_bits $f
    check "$codec $rate Hz, $channels channels, $rate_bits: against ffmpeg" $WC against $f $WX_FLOOR
    check "$codec $rate Hz, $channels channels, $rate_bits: random reads equal the straight decode" python3 audio.py chunks adecode $f $WX_READS 5
done <<TABLE
wmav1 8000 1 24k
wmav1 11025 2 32k
wmav1 16000 2 32k
wmav1 22050 2 24k
wmav1 32000 1 64k
wmav1 44100 2 128k
wmav1 48000 1 48k
wmav2 8000 1 32k
wmav2 12000 1 24k
wmav2 16000 2 48k
wmav2 22050 2 32k
wmav2 24000 2 24k
wmav2 32000 2 48k
wmav2 44100 1 128k
wmav2 44100 2 32k
wmav2 44100 2 48k
wmav2 44100 2 320k
wmav2 48000 2 24k
wmav2 48000 2 192k
TABLE
$F -f lavfi -i "aevalsrc=$WX_STEREO:s=44100:d=$WX_SECONDS" -c:a wmav2 -b:a 96k $O/wx_direct.mkv
$F -f lavfi -i "aevalsrc=$WX_STEREO:s=22050:d=$WX_SECONDS" -c:a wmav1 -b:a 48k $O/wx_direct.avi
check "Matroska written by ffmpeg (its own start skip) against ffmpeg" $WC against $O/wx_direct.mkv $WX_FLOOR
check "AVI written by ffmpeg against ffmpeg" $WC against $O/wx_direct.avi $WX_FLOOR
for f in wma2_48k.mkv wma1_32k.mkv wma2_noise.wma; do check "probe fields of $f" python3 probe_check.py compare $WX/$f; done
check "probe fields of wma2_48k.avi (time base, length and bit rate from the data, not the stream header)" python3 probe_check.py compare $WX/wma2_48k.avi stream0.time_base stream0.duration format.duration format.bit_rate

echo "== wma: Windows Media Audio 9 Professional against ffmpeg's decoder (SNR floor $WX_FLOOR dB, lengths to the sample)"
for f in $PX/pro_*; do check "$(basename $f) against ffmpeg" $WC against $f $WX_FLOOR; done
for f in pro_71.wma pro_51side.wma pro_stereo24.wma pro_quad.mkv; do check "probe fields of $f" python3 probe_check.py compare $PX/$f stream0.duration; done

echo "== wma: Windows Media Audio 9 Lossless bit-exact against ffmpeg's decoder"
for f in $LX/lossless_*; do check "$(basename $f) bit-exact against ffmpeg" $WC exact $f; done
for f in lossless_51.wma lossless_stereo24.wma lossless_mono.mkv; do check "probe fields of $f" python3 probe_check.py compare $LX/$f stream0.duration; done

echo "== wma: speed against ffmpeg on one thread (ours at most 1.5 times its time)"
WX_LONG=600
[ -n "${QUICK:-}" ] && WX_LONG=180
WX_PAIR="sine=f=330:r=44100:d=$WX_LONG[a];anoisesrc=d=$WX_LONG:r=44100:a=0.1:c=pink[b];[a][b]amerge=inputs=2"
$F -filter_complex "$WX_PAIR" -c:a wmav2 -b:a 128k $O/wx_long_128k.wma
$F -filter_complex "$WX_PAIR" -c:a wmav2 -b:a 32k $O/wx_long_32k.wma
check "wmav2 128 kbit/s stereo, $WX_LONG s" $WC speed $O/wx_long_128k.wma 1.5
check "wmav2 32 kbit/s stereo with noise coding, $WX_LONG s" $WC speed $O/wx_long_32k.wma 1.5
check "a jump to the end of the noise-coded file is exact (the decoder state is rebuilt by parsing only)" python3 audio.py chunks adecode $O/wx_long_32k.wma 4 7

if [ -n "${WINMEDIA_SAMPLES:-}" ] && [ -d "$WINMEDIA_SAMPLES" ]; then
    echo "== wma: Microsoft-made sample files from WINMEDIA_SAMPLES (not committed)"
    for f in $(cd "$WINMEDIA_SAMPLES" && ls *.wmv *.wma *.asf fate/*/*.wma 2> /dev/null); do
        kind=$($A probe --json "$WINMEDIA_SAMPLES/$f" 2> /dev/null | grep -o -E '"codec_name":"(wmav[12]|wmapro|wmalossless)"')
        [ -n "$kind" ] || continue
        case "$kind:$f" in
            *wmalossless*:fate/*) check "$f bit-exact against ffmpeg (but the last frame, which reads past the cut)" $WC exact "$WINMEDIA_SAMPLES/$f" 2048 ;;
            *wmalossless*) check "$f bit-exact against ffmpeg" $WC exact "$WINMEDIA_SAMPLES/$f" ;;
            *) check "$f against ffmpeg" $WC against "$WINMEDIA_SAMPLES/$f" $WX_FLOOR ;;
        esac
        check "$f: random reads equal the straight decode" python3 audio.py chunks adecode "$WINMEDIA_SAMPLES/$f" $WX_READS 9
    done
    for f in nintendothemesacappellax.wmv kelsy_300.wmv wandue_project_-_king_of_my_castle.asf Classical_44_16_6_256000_0_20.wma Classical_96_24_2_Q75_2_3.wma Robotica_1080.wmv luckynight.wma wma_0x163.wma; do
        [ -f "$WINMEDIA_SAMPLES/$f" ] && check "speed on $f" $WC speed "$WINMEDIA_SAMPLES/$f" 1.5
    done
fi
rm -rf $O/wx_*
