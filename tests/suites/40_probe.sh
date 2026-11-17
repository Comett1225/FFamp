echo "== probe: fixtures against the committed expected output (no FFmpeg)"
PX=fixtures/probe
pinned() { [ "$("${@:3}" | md5)" = "$(grep " $1 $2\$" $PX/checksums.txt | cut -d' ' -f1)" ]; }
for f in $PX/*.mp4 $PX/*.mov $PX/*.m4a $PX/*.ts $PX/*.m2ts; do
    name=$(basename $f)
    check "probe $name gives the expected JSON" python3 probe_check.py expect $f $PX/expected/$name.json
    check "probe $name packet list has the checked checksum" pinned packets $name $A probe --json --packets $f
    tracks=$(grep -c " track[0-9]* $name\$" $PX/checksums.txt)
    for ((t = 0; t < tracks; t++)); do check "$name track $t packets have the checked checksum" pinned track$t $name $A dump $f --track $t; done
done

echo "== transport stream: decode, same packets as the MP4 they were copied from, remux (no FFmpeg)"
for f in h264_aac.ts hevc_aac.ts h264.m2ts open_mp2.ts wrap.ts h264_mp3.ts; do check "decode $f matches the checked checksum" pinned decode $f $A decode $PX/$f; done
check "h264_aac.ts decodes to the same frames as h264.mp4" [ "$($A decode $PX/h264_aac.ts | md5)" = "$($A decode $PX/h264.mp4 | md5)" ]
check "h264.m2ts decodes to the same frames as h264.mp4" [ "$($A decode $PX/h264.m2ts | md5)" = "$($A decode $PX/h264.mp4 | md5)" ]
check "h264_aac.ts AAC packets are the ones in h264.mp4" [ "$($A dump $PX/h264_aac.ts --track 1 | md5)" = "$($A dump $PX/h264.mp4 --track 1 | md5)" ]
check "h264.m2ts AAC packets are the ones in h264.mp4" [ "$($A dump $PX/h264.m2ts --track 1 | md5)" = "$($A dump $PX/h264.mp4 --track 1 | md5)" ]
for f in h264_aac.ts hevc_aac.ts h264.m2ts; do
    $A remux $PX/$f $O/ts_remux_$f.mp4
    check "$f remuxed to MP4 opens in AVFoundation" ../build/avcheck $O/ts_remux_$f.mp4
    check "$f remuxed to MP4 decodes to the same frames" [ "$($A decode $O/ts_remux_$f.mp4 | md5)" = "$($A decode $PX/$f | md5)" ]
done

echo "== transport stream: hand-made streams (no FFmpeg)"
SY=$O/synthetic
mkdir -p $SY
same_tracks() { [ "$($A dump $1 --track 0 | md5)" = "$($A dump $2 --track 0 | md5)" ] && [ "$($A dump $1 --track 1 | md5)" = "$($A dump $2 --track 1 | md5)" ]; }
python3 ts_make.py fields $SY/fields.ts
check "two fields of one frame become one sample" [ "$($A probe $SY/fields.ts | grep -c 'frames 6 ')" = 1 ]
python3 ts_make.py tiny $PX/h264_aac.ts $SY/tiny.ts
check "packets of 1 to 11 payload bytes (headers and start codes split) give the same samples" same_tracks $SY/tiny.ts $PX/h264_aac.ts
python3 ts_make.py wide $PX/h264_aac.ts $SY/wide.ts
check "204-byte packets give the same samples" same_tracks $SY/wide.ts $PX/h264_aac.ts
python3 ts_make.py twice $PX/h264_aac.ts $SY/twice.ts
check "repeated packets give the same samples" same_tracks $SY/twice.ts $PX/h264_aac.ts
python3 ts_make.py spans $PX/h264_aac.ts $SY/spans.ts
check "AAC frames split across PES packets give the same samples" same_tracks $SY/spans.ts $PX/h264_aac.ts
broken=$(python3 ts_make.py lost $PX/h264_aac.ts $SY/lost.ts)
check "a lost packet drops that frame and the rest of its group only" [ "$($A probe $SY/lost.ts | grep -c "frames $((broken + 30)) ")" = 1 ]
check "the stream with the lost packet still decodes" [ "$($A decode $SY/lost.ts | wc -c | tr -d ' ')" = $(((broken + 30) * 320 * 180 * 3 / 2)) ]

echo "== damaged files: cut, overwritten and missing parts never crash (no FFmpeg)"
mkdir -p $O/damaged
for f in h264_aac.ts hevc_aac.ts h264.m2ts open_mp2.ts h264_eac3.ts wrap.ts h264.mp4 hevc.mov frag.mp4 edits.mp4 tags.m4a; do
    check "damaged copies of $f" python3 probe_check.py damage $PX/$f $O/damaged ${DAMAGE_ROUNDS:-40} 7
done

have_ffmpeg || { skip "probe fields and packets against ffprobe"; skip "transport stream decode and seek against ffmpeg"; return 0; }

echo "== probe: every field the app reads against ffprobe"
TS_KNOWN="stream1.duration~0.3 format.duration~0.05"
for f in h264.mp4 hevc.mov frag.mp4 rot90.mp4 rot180.mp4 rot270.mp4 delay.mp4 delay_frag.mp4 vfr.mp4 hdr10.mp4 full.mp4 ntsc.mp4 colr.mp4 tags.m4a; do
    check "fields of $f" python3 probe_check.py compare $PX/$f
    check "packets of $f" python3 probe_check.py packets $PX/$f
done
check "fields of edits.mp4 (first segment only, by design)" python3 probe_check.py compare $PX/edits.mp4 stream0.duration
for f in h264_aac.ts hevc_aac.ts h264.m2ts wrap.ts; do
    check "fields of $f" python3 probe_check.py compare $PX/$f $TS_KNOWN stream1.bit_rate~0.04
    check "packets of $f" python3 probe_check.py packets $PX/$f stream0.size stream1.size+7 stream1.pos stream1.pts~1 stream1.dts~1
done
for f in h264_mp3.ts h264_ac3.ts h264_eac3.ts; do
    check "fields of $f" python3 probe_check.py compare $PX/$f $TS_KNOWN
    check "packets of $f" python3 probe_check.py packets $PX/$f stream0.size stream1.pos stream1.pts~1 stream1.dts~1
done
check "fields of open_mp2.ts" python3 probe_check.py compare $PX/open_mp2.ts $TS_KNOWN
check "packets of open_mp2.ts (key flags on IDR only)" python3 probe_check.py packets $PX/open_mp2.ts stream0.size stream0.flags stream1.pos stream1.pts~1 stream1.dts~1

echo "== transport stream: every frame and random seeks against ffmpeg"
$F -i $O/h264_720.mp4 -c copy $O/h264_720.ts
$F -i $O/hevc_720.mp4 -c copy $O/hevc_720.ts
$F -i $O/h264_720.mp4 -c copy -f mpegts -mpegts_m2ts_mode 1 $O/h264_720.m2ts
for f in $O/h264_720.ts $O/hevc_720.ts $O/h264_720.m2ts $PX/open_mp2.ts $PX/wrap.ts; do
    check "decode $(basename $f)" python3 compare.py decode $f
    check "seek $(basename $f)" python3 compare.py seek $f 30 7
done
check "packets of h264_720.ts" python3 probe_check.py packets $O/h264_720.ts stream0.size stream1.size+7 stream1.pos stream1.pts~1 stream1.dts~1
check "packets of hevc_720.ts" python3 probe_check.py packets $O/hevc_720.ts stream0.size stream1.size+7 stream1.pos stream1.pts~1 stream1.dts~1

echo "== probe: other containers against ffprobe"
other() {
    $F "${@:2}" $O/$1
    if $A probe --json $O/$1 > /dev/null 2>&1; then check "fields of $1" python3 probe_check.py compare $O/$1 ${OTHER_KNOWN:-}; else skip "fields of $1 (no reader yet)"; fi
}
other probe_h264_opus.mkv -i $PX/h264.mp4 -c:v copy -c:a libopus -b:a 64k
other probe_vp9.webm -f lavfi -i testsrc2=size=160x90:rate=30 -t 1 -c:v libvpx-vp9 -b:v 200k
other probe_opus.ogg -f lavfi -i sine=frequency=440:sample_rate=48000 -t 1 -c:a libopus -b:a 64k
other probe_s16.wav -f lavfi -i sine=frequency=440:sample_rate=44100 -t 1 -ac 2 -c:a pcm_s16le
other probe_lame.mp3 -f lavfi -i sine=frequency=440:sample_rate=44100 -t 1 -c:a libmp3lame -b:a 64k
other probe_s16.flac -f lavfi -i sine=frequency=440:sample_rate=44100 -t 1 -c:a flac
other probe_late_sound.mov -f lavfi -i testsrc2=size=320x240:rate=30:duration=2 -itsoffset 0.5 -f lavfi -i sine=frequency=440:sample_rate=48000:duration=1.5 -c:v libx264 -c:a aac
other probe_late_picture.mp4 -itsoffset 0.5 -f lavfi -i testsrc2=size=320x240:rate=30:duration=2 -f lavfi -i sine=frequency=440:sample_rate=48000:duration=1.5 -c:v libx264 -c:a aac
OTHER_KNOWN="stream0.avg_frame_rate duration format.bit_rate" other probe_late_sound.wmv -f lavfi -i testsrc2=size=320x240:rate=30:duration=2 -itsoffset 0.5 -f lavfi -i sine=frequency=440:sample_rate=48000:duration=1.5 -c:v wmv2 -c:a wmav2

echo "== probe: committed reader, audio and video fixtures against ffprobe"
RXP=fixtures/readers
for f in aac.mka ac3.mka alac.mka av1_opus.webm bframes.mkv eac3.mka f32le.mka flac.mka h264_aac.mkv hevc_opus.mkv mp3.mka prores_pcm.mkv s16be.mka s24le.mka vp8_vorbis.webm vp9_opus.webm \
    cbr.mp3 id3v23.mp3 id3v24.mp3 padded.mp3 plain.mp3 layer2.mp2 deep.flac fixed.flac variable.flac f32.wav f64.wav rf64.wav s16.wav s24.wav s32.wav six.wav u8.wav \
    fl32.aifc fl64.aifc s16.aiff s24.aiff sowt.aifc; do
    check "fields of $f" python3 probe_check.py compare $RXP/$f
done
check "fields of opus.ogg (tags are read from the stream too)" python3 probe_check.py compare $RXP/opus.ogg tag.title tag.artist tag.album
check "fields of vbr.mp3 (bit rate from the packets, not the header)" python3 probe_check.py compare $RXP/vbr.mp3 stream0.bit_rate
for f in fixtures/audio/pcm_*.mov fixtures/audio/pcm_*.mp4 fixtures/video/av1_270p.mp4 fixtures/video/av1_10bit_270p.mp4 fixtures/video/prores_180p.mov; do
    check "fields of $(basename $f)" python3 probe_check.py compare $f
done
