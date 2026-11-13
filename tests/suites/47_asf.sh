echo "== asf: Windows Media files against checked tables (no FFmpeg)"
AX=fixtures/asf
AC="python3 $AX/craft.py"
AK="python3 asf_check.py"
AX_ALL="wma1.wma wma2.wma wmv1_wma2.wmv wmv2.wmv msmpeg4v3.asf msmpeg4v2.asf h264_mp3.asf mpeg4_pcm.asf avi_div3.avi avi_wmv2.avi"
ax_listed() { [ "$("${@:3}" 2> /dev/null | md5)" = "$(grep " $2\$" $AX/$1.md5 | cut -d' ' -f1)" ]; }
ax_refuses() { ! $A $1 $2 > /dev/null 2> $O/ax_refused.txt && grep -q "$3" $O/ax_refused.txt; }
ax_packets() { $A packets $1 | grep -E '^-?[0-9]'; }
ax_differs() { ! cmp -s "$1" "$2"; }
ax_same() { [ "$($A packets $1 | md5)" = "$($A packets $2 | md5)" ]; }
for f in $AX_ALL; do
    check "packets of $f match the checked table" ax_listed packets $f $A packets $AX/$f
    check "probe of $f matches the checked output" ax_listed probe $f $A probe --json $AX/$f
done
check "h264_mp3.asf pictures decode to the checked checksum" ax_listed pictures h264_mp3.asf $A decode $AX/h264_mp3.asf
for f in h264_mp3.asf mpeg4_pcm.asf; do check "random frames of $f equal the straight decode" python3 comparex.py selfseek $AX/$f 10 5; done
for f in h264_mp3.asf mpeg4_pcm.asf msmpeg4v3.asf; do
    check "$f sound decodes to the checked checksum" ax_listed sound $f $A adecode $AX/$f
    check "random reads $f" python3 audio.py chunks adecode $AX/$f 30 3
done
while read f name; do
    check "probe names $f $name" grep -q "\"codec_name\":\"$name\"" <($A probe --json $AX/$f)
done <<TABLE
wma1.wma wmav1
wma2.wma wmav2
wmv1_wma2.wmv wmv1
wmv2.wmv wmv2
msmpeg4v3.asf msmpeg4v3
msmpeg4v2.asf msmpeg4v2
avi_div3.avi msmpeg4v3
avi_wmv2.avi wmv2
TABLE
while read command f text; do
    check "$f: $command says the codec is not decoded yet" ax_refuses $command $AX/$f "$text) is not decoded yet"
done <<TABLE
adecode wma1.wma Windows Media Audio 1
adecode wma2.wma Windows Media Audio 2
adecode wmv1_wma2.wmv Windows Media Audio 2
decode wmv1_wma2.wmv Windows Media Video 7
decode wmv2.wmv Windows Media Video 8
decode msmpeg4v3.asf Microsoft MPEG-4 version 3
decode msmpeg4v2.asf Microsoft MPEG-4 version 2
decode avi_div3.avi Microsoft MPEG-4 version 3 (DivX 3)
decode avi_wmv2.avi Windows Media Video 8
TABLE
check "title, artist, album, date and track tags of wma2.wma" [ "$($A packets $AX/wma2.wma | grep -c -E '^tag (title=Title|artist=Artist|album=Album|date=2004|track=3)$')" = 5 ]

echo "== asf: altered copies of the fixtures (no FFmpeg)"
$AC spread $AX/mpeg4_pcm.asf $O/ax_spread.asf 4 512 128
check "scrambled (spread) sound is put back in its order" [ "$($A packets $O/ax_spread.asf --raw --track 2 | md5)" = "$($A packets $AX/mpeg4_pcm.asf --raw --track 2 | md5)" ]
check "the scrambled copy really is scrambled" ax_differs $O/ax_spread.asf $AX/mpeg4_pcm.asf
$AC explicit $AX/wma2.wma $O/ax_explicit.wma
check "packets that state their own length give the same media objects" ax_same $O/ax_explicit.wma $AX/wma2.wma
check "the copy really states packet lengths" ax_differs $O/ax_explicit.wma $AX/wma2.wma
$AC keyless $AX/wmv2.wmv $O/ax_keyless.wmv
check "with no key frame marks, the simple index gives the same key frames" $AK keys $O/ax_keyless.wmv $AX/wmv2.wmv 0
$AC plain $AX/wmv2.wmv $O/ax_plain.wmv
check "with no marks and no index, the first picture is the only key frame" [ "$($A packets $O/ax_plain.wmv | grep -c ' K ')" = 1 ]
$AC plain $AX/h264_mp3.asf $O/ax_avc.asf
check "H.264 with no marks and no index finds its key frames in the pictures" ax_same $O/ax_avc.asf $AX/h264_mp3.asf
$AC drm-flag $AX/wma1.wma $O/ax_drm1.wma
$AC drm-object $AX/wma2.wma $O/ax_drm2.wma
check "a stream marked as encrypted is refused with the DRM message" ax_refuses packets $O/ax_drm1.wma "protected by DRM"
check "a file with a content encryption object is refused with the DRM message" ax_refuses probe $O/ax_drm2.wma "protected by DRM"
head -c 30000 $AX/wmv2.wmv > $O/ax_cut.wmv
ax_prefix() { [ "$(ax_packets $O/ax_cut.wmv | md5)" = "$(ax_packets $AX/wmv2.wmv | head -n $(ax_packets $O/ax_cut.wmv | wc -l) | md5)" ]; }
check "a cut-off file gives the packets before the cut" ax_prefix
check "and probe gives the length that is there, not the stated one" python3 -c "import json,subprocess,sys; d=json.loads(subprocess.run(['$A','probe','--json','$O/ax_cut.wmv'],capture_output=True,text=True).stdout); sys.exit(0 if float(d['format']['duration']) < 3 else 1)"

echo "== asf: damaged files never crash (no FFmpeg)"
AX_FUZZ=30
[ -n "${QUICK:-}" ] && AX_FUZZ=6
check "cut, flipped, shortened and padded ASF files never crash" python3 fuzz.py $O/ax_fuzz.bin $AX_FUZZ $AX/*.asf $AX/*.wma $AX/*.wmv $O/ax_spread.asf $O/ax_explicit.wma
rm -rf $O/ax_fz
mkdir -p $O/ax_fz/seeds $O/ax_fz/corpus
cp $AX/*.asf $AX/*.wma $AX/*.wmv $O/ax_spread.asf $O/ax_explicit.wma $O/ax_drm1.wma $O/ax_drm2.wma $O/ax_cut.wmv $O/ax_fz/seeds/
for target in packets probe; do
    check "fuzz_$target: ${FUZZ_BURST:-3} s of mutated ASF files, no crash, hang or memory blow-up" env TMPDIR=$O/ax_fz ../build/fuzz_$target -seed=11 -max_total_time=${FUZZ_BURST:-3} -corpus_limit_mb=0 -rss_limit_mb=2048 -artifact_prefix=$O/ax_fz/ $O/ax_fz/corpus $O/ax_fz/seeds 2> $O/ax_fz/log.txt
done

have_ffmpeg || { rm -rf $O/ax_*; skip "asf against ffmpeg and ffprobe"; return 0; }

echo "== asf: every packet against ffmpeg's demux"
check "bytes, times and key flags of every packet in the fixtures and the altered copies" $AK packets $AX/*.asf $AX/*.wma $AX/*.wmv $O/ax_spread.asf $O/ax_explicit.wma
check "seeking in wmv2.wmv: ffmpeg starts on one of our key frames, at or before ours" $AK seek $AX/wmv2.wmv 10
AX_ALLOW="stream0.duration stream1.duration format.duration bit_rate avg_frame_rate r_frame_rate"
for f in wma1.wma wma2.wma wmv1_wma2.wmv wmv2.wmv msmpeg4v3.asf msmpeg4v2.asf mpeg4_pcm.asf; do
    check "fields of $f (each stream's length is its own, not the file's)" python3 probe_check.py compare $AX/$f $AX_ALLOW
done
check "fields of h264_mp3.asf (ffprobe starts the H.264 track at the first sound packet)" python3 probe_check.py compare $AX/h264_mp3.asf $AX_ALLOW stream0.start_time format.start_time
for f in avi_div3.avi avi_wmv2.avi; do check "fields of $f" python3 probe_check.py compare $AX/$f bit_rate; done
check "decode h264_mp3.asf (B-frames put in order) frame-exact" python3 comparex.py decode $AX/h264_mp3.asf
check "decode mpeg4_pcm.asf" python3 comparex.py decode $AX/mpeg4_pcm.asf 45
check "h264_mp3.asf sound against ffmpeg" python3 audio.py against $AX/h264_mp3.asf 75 0 0
check "msmpeg4v3.asf sound against ffmpeg" python3 audio.py against $AX/msmpeg4v3.asf 60 0 0
check "mpeg4_pcm.asf sound bit-exact against ffmpeg" python3 audio.py against $AX/mpeg4_pcm.asf 200 0 0 exact
check "the scrambled copy's sound bit-exact against ffmpeg" python3 audio.py against $O/ax_spread.asf 200 0 0 exact

if [ -n "${WINMEDIA_SAMPLES:-}" ] && [ -d "$WINMEDIA_SAMPLES" ]; then
    echo "== asf: Microsoft-made sample files from WINMEDIA_SAMPLES (not committed)"
    AX_SAMPLES=$(cd "$WINMEDIA_SAMPLES" && ls *.wmv *.wma *.asf fate/*/*.wma fate/*/*.wmv 2> /dev/null | grep -v -e fault -e drm)
    for f in $AX_SAMPLES; do
        check "packets of $f against ffmpeg's demux" $AK packets "$WINMEDIA_SAMPLES/$f"
        check "fields of $f" python3 probe_check.py compare "$WINMEDIA_SAMPLES/$f" $AX_ALLOW language format.start_time stream0.start_time stream1.start_time stream2.duration
    done
    [ -f "$WINMEDIA_SAMPLES/fate/wmv8/wmv_drm.wmv" ] && check "a DRM-protected sample is refused with the DRM message" ax_refuses probe "$WINMEDIA_SAMPLES/fate/wmv8/wmv_drm.wmv" "protected by DRM"
    for f in Robotica_720.wmv kelsy_300.wmv sample-wmv7.wmv Park_auto_no_driver.wmv nintendothemesacappellax.wmv; do
        [ -f "$WINMEDIA_SAMPLES/$f" ] || continue
        $AC keyless "$WINMEDIA_SAMPLES/$f" $O/ax_sample.wmv
        check "$f with its key marks removed: key frames from the index (at most 2 % wrong)" $AK keys $O/ax_sample.wmv "$WINMEDIA_SAMPLES/$f" 0.02
        check "seeking in $f" $AK seek "$WINMEDIA_SAMPLES/$f" 10
    done
fi
rm -rf $O/ax_*
