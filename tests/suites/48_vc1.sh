echo "== VC-1: WMV3 fixtures written by fixtures/vc1/make.py (Main with B, intensity compensation, range reduction; Simple) (no FFmpeg)"
VC=fixtures/vc1
vc_listed() { [ "$("${@:3}" 2> /dev/null | md5)" = "$(grep " $2\$" $VC/$1.md5 | cut -d' ' -f1)" ]; }
vc_backend() { $A videoinfo "$1" | grep -q "backend vc1"; }
vc_refuses() { ! $A decode "$1" > /dev/null 2> $O/vc_refused.txt && grep -q "$2" $O/vc_refused.txt; }
vc_threads() { [ "$($A decode "$1" --threads 6 | md5)" = "$($A decode "$1" | md5)" ]; }
vc_leaks() { leaks --atExit -- $A frames "$1" --list 29,3,17,0,12,28,1 2> /dev/null | LC_ALL=C grep -aq ": 0 leaks for 0 total leaked bytes"; }
vc_remade() { rm -rf $O/vc_made && mkdir -p $O/vc_made/vc1 && python3 $VC/make.py $O/vc_made/vc1 && cmp -s $O/vc_made/vc1/main_bframes.avi $VC/main_bframes.avi && cmp -s $O/vc_made/vc1/simple.avi $VC/simple.avi && cmp -s $O/vc_made/fuzz/vc1/simple.bin fixtures/fuzz/vc1/simple.bin; }
vc_shown() { [ "$($A packets $VC/main_bframes.avi | grep -E '^[0-9]' | cut -d' ' -f1 | tr '\n' ' ')" = "0 2 1 4 3 6 5 7 9 8 11 10 " ]; }
VC_FUZZ=20
[ -n "${QUICK:-}" ] && VC_FUZZ=6
for f in main_bframes.avi simple.avi; do
    check "$f decodes with FFamp's own VC-1 decoder" vc_backend $VC/$f
    check "$f pictures decode to the checked checksum" vc_listed pictures $f $A decode $VC/$f
    check "random frames of $f equal the straight decode" python3 comparex.py selfseek $VC/$f 10 5
    check "damaged copies of $f decode or fail cleanly" python3 comparex.py fuzz $VC/$f $VC_FUZZ 9
done
check "make.py writes the committed fixtures again byte for byte" vc_remade
check "B-pictures in AVI get their shown order from the picture types" vc_shown
check "six threads decoding the B-picture fixture at once each get the straight decode" vc_threads $VC/main_bframes.avi
if command -v leaks > /dev/null; then
    check "jumping around the B-picture fixture leaks nothing (B-pictures go to worker threads)" eval "leaks --atExit -- $A frames $VC/main_bframes.avi --list 9,3,11,0,6,2,10,1 2> /dev/null | LC_ALL=C grep -aq ': 0 leaks for 0 total leaked bytes'"
fi
if have_ffmpeg; then
    for f in main_bframes.avi simple.avi; do
        check "decode $f bit-exact against ffmpeg" python3 vc1_check.py ordered $VC/$f
        check "random frames of $f against ffmpeg" python3 comparex.py seek $VC/$f 8 7
    done
fi
rm -rf $O/vc_made

echo "== VC-1: WMV3 Simple, Main and Complex (no X8), WVC1 and WMVA Advanced from WINMEDIA_SAMPLES (not committed)"
VS="${WINMEDIA_SAMPLES:-}"
if [ -z "$VS" ] || [ ! -d "$VS" ]; then
    skip "VC-1 sample files (set WINMEDIA_SAMPLES to the sample folder)"
    return 0
fi
VC_FILES="sample-wmv9-simple-SP@ML.wmv sample-wmv9-main-MP@LL.wmv sb-spl-q94.avi sb-main-q94.avi sb-adv-q94.avi Robotica_720.wmv Robotica_1080.wmv 480i30__codec_WVC1__mode_2__framerate_29.970__type_2__preproc_17.wmv wmva-smpl-bdsm.wmv"
VC_SMALL="sample-wmv9-main-MP@LL.wmv sb-adv-q94.avi 480i30__codec_WVC1__mode_2__framerate_29.970__type_2__preproc_17.wmv wmva-smpl-bdsm.wmv"
for f in $VC_FILES; do
    [ -f "$VS/$f" ] || continue
    check "$f decodes with FFamp's own VC-1 decoder" vc_backend "$VS/$f"
    check "$f pictures decode to the checked checksum" vc_listed pictures $f $A decode "$VS/$f"
done
for f in $VC_SMALL; do
    [ -f "$VS/$f" ] || continue
    check "random frames of $f equal the straight decode" python3 comparex.py selfseek "$VS/$f" 16 5
done
for f in sample-wmv9-complex-CP.wmv kelsy_300.wmv; do
    [ -f "$VS/$f" ] && check "$f (X8 intra pictures) is refused with its own message" vc_refuses "$VS/$f" "X8 intra pictures"
done
[ -f "$VS/Robotica_1080.wmv" ] && check "1440x1080 WMV3 Main decode into memory: over 230 frames per second (the plan's floor)" python3 speedcheck.py rate 480 230 bench "$VS/Robotica_1080.wmv" --mode copy
for f in sample-wmv9-main-MP@LL.wmv 480i30__codec_WVC1__mode_2__framerate_29.970__type_2__preproc_17.wmv; do
    [ -f "$VS/$f" ] && check "runs of frames after jumps in $f equal the straight decode" python3 speedcheck.py steps "$VS/$f"
done
f="$VS/480i30__codec_WVC1__mode_2__framerate_29.970__type_2__preproc_17.wmv"
[ -f "$f" ] && check "six threads decoding an interlaced VC-1 file at once each get the straight decode" vc_threads "$f"
if command -v leaks > /dev/null && [ -f "$VS/sample-wmv9-main-MP@LL.wmv" ]; then
    check "jumping around a VC-1 file leaks nothing" vc_leaks "$VS/sample-wmv9-main-MP@LL.wmv"
fi
for f in sample-wmv9-main-MP@LL.wmv sb-adv-q94.avi 480i30__codec_WVC1__mode_2__framerate_29.970__type_2__preproc_17.wmv; do
    [ -f "$VS/$f" ] && check "damaged copies of $f decode or fail cleanly" python3 comparex.py fuzz "$VS/$f" $VC_FUZZ 9
done

have_ffmpeg || { skip "VC-1 against ffmpeg"; return 0; }

echo "== VC-1 against ffmpeg's decode, every frame"
for f in $VC_FILES; do
    [ -f "$VS/$f" ] || continue
    case $f in
        wmva-*) check "decode $f bit-exact against ffmpeg (ffmpeg also decodes the cut-off last media object)" python3 vc1_check.py ordered "$VS/$f" 1 ;;
        *) check "decode $f bit-exact against ffmpeg" python3 vc1_check.py ordered "$VS/$f" ;;
    esac
done
for f in $VC_SMALL; do
    [ -f "$VS/$f" ] && check "random frames of $f against ffmpeg" python3 comparex.py seek "$VS/$f" 12 7
done
f="$VS/480i30__codec_WVC1__mode_2__framerate_29.970__type_2__preproc_17.wmv"
[ -f "$f" ] && check "Advanced profile range mapping (RANGE_MAPY, RANGE_MAPUV) is applied to the output as ST 421 says (ffmpeg ignores it)" python3 vc1_check.py rangemap "$f" $O/vc_range.mkv
for f in sample-wmv9-main-MP@LL.wmv sb-main-q94.avi Robotica_720.wmv; do
    [ -f "$VS/$f" ] && check "frame times of $f equal ffprobe's" python3 formats_check.py frametimes "$VS/$f"
done
if [ -d "$VS/fate/vc1" ]; then
    echo "== VC-1 elementary streams from FATE (SMPTE-style conformance files), put in Matroska by ffmpeg"
    for f in "$VS"/fate/vc1/*.vc1 "$VS"/fate/vc1/*.rcv; do
        n=$(basename "$f")
        $F -i "$f" -c copy $O/vc_${n%.*}.mkv 2> /dev/null
        check "$n: every picture equals ffmpeg's (the raw stream gives no display times, so the order is not compared)" python3 vc1_check.py unordered $O/vc_${n%.*}.mkv
    done
    if [ -f "$VS/fate/vc1/SMM0005.rcv" ]; then
        $F -i "$VS/fate/vc1/SMM0005.rcv" -c copy $O/vc_bframes.avi 2> /dev/null
        check "WMV3 with B-frames in AVI: shown order from the picture types, every frame bit-exact in order" python3 vc1_check.py ordered $O/vc_bframes.avi
        check "random frames of the WMV3 B-frame AVI against ffmpeg" python3 comparex.py seek $O/vc_bframes.avi 8 7
    fi
fi
rm -f $O/vc_*
