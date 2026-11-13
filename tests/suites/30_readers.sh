echo "== readers: fixtures against checked tables"
RX=fixtures/readers
table_matches() { [ "$($A packets "$RX/$1" | md5)" = "$(grep " $1\$" $RX/packets.md5 | cut -d' ' -f1)" ]; }
while read -r sum name; do check "packets of $name match the checked table" table_matches "$name"; done < $RX/packets.md5

echo "== readers: the same stream in different wrappings"
columns() { $A packets "$1" | grep -E '^-?[0-9]' | cut -d' ' -f"$2" | md5; }
same_columns() { [ "$(columns "$1" "$3")" = "$(columns "$2" "$3")" ]; }
trim_of() { $A packets "$1" | grep '^track' | cut -d' ' -f4-10 | md5; }
same_trim() { [ "$(trim_of "$1")" = "$(trim_of "$2")" ]; }
check "Ogg Opus cut into one-segment pages gives the same packets" same_columns $RX/opus.ogg $RX/opus_small_pages.ogg 1-6
check "Ogg FLAC cut into small pages gives the same packets" same_columns $RX/flac.ogg $RX/flac_small_pages.ogg 1-6
check "a chained Ogg file gives its first stream" same_columns $RX/opus.ogg $RX/chained.ogg 1-6
check "FLAC frames are the same from FLAC and from Ogg" same_columns $RX/fixed.flac $RX/flac.ogg 1-6
check "FLAC frames are the same from FLAC and from Matroska" same_columns $RX/fixed.flac $RX/flac.mka 1-6
check "FLAC with coded sample numbers has the same times" same_columns $RX/fixed.flac $RX/variable.flac 1-3
check "ADTS with checksums gives the same AAC packets" same_columns $RX/adts.aac $RX/adts_checksum.aac 1-6
check "MP3 behind leading junk gives the same packets" same_columns $RX/plain.mp3 $RX/padded.mp3 1-6
check "MP3 behind a VBRI header frame gives the same packets" same_columns $RX/plain.mp3 $RX/vbri.mp3 1-6
check "Xiph and EBML lacing give the same packets" same_columns $RX/lace_xiph.mka $RX/lace_ebml.mka 1-6
check "stripped headers are put back" same_columns $RX/lace_xiph.mka $RX/stripped.mka 1-6
check "fixed lacing gives the MP3 file's packets" same_columns $RX/plain.mp3 $RX/lace_fixed.mka 3-6

echo "== readers: video from Matroska decodes frame-exact"
decodes_to() { [ "$($A decode "$1" | md5)" = "$2" ]; }
H264_SUM=$(grep ' h264_270p.mp4$' fixtures/video/decoded.md5 | cut -d' ' -f1)
HEVC_SUM=$(grep ' hevc_270p.mp4$' fixtures/video/decoded.md5 | cut -d' ' -f1)
BFRAMES_SUM=$(grep ' bframes.mkv$' $RX/decoded.md5 | cut -d' ' -f1)
check "H.264 in Matroska decodes to the MP4 fixture's frames" decodes_to $RX/h264_aac.mkv $H264_SUM
check "HEVC in Matroska decodes to the MP4 fixture's frames" decodes_to $RX/hevc_opus.mkv $HEVC_SUM
check "H.264 with B-frames in Matroska decodes to the checked frames" decodes_to $RX/bframes.mkv $BFRAMES_SUM

echo "== readers: stream copy into our writers and back"
$A remux $RX/h264_aac.mkv $O/rd_h264.mkv
check "Matroska to Matroska keeps every packet" same_columns $RX/h264_aac.mkv $O/rd_h264.mkv 1-6
check "Matroska to Matroska still decodes to the same frames" decodes_to $O/rd_h264.mkv $H264_SUM
$A remux $RX/h264_aac.mkv $O/rd_h264.mp4 --video-only
check "Matroska H.264 copied to MP4 opens in AVFoundation" ../build/avcheck $O/rd_h264.mp4
check "Matroska H.264 copied to MP4 decodes to the same frames" decodes_to $O/rd_h264.mp4 $H264_SUM
$A remux $RX/bframes.mkv $O/rd_bframes.mp4
check "B-frame Matroska copied to MP4 opens in AVFoundation" ../build/avcheck $O/rd_bframes.mp4
check "B-frame Matroska copied to MP4 decodes to the same frames" decodes_to $O/rd_bframes.mp4 $BFRAMES_SUM
$A remux $RX/hevc_opus.mkv $O/rd_hevc.mkv
check "HEVC and Opus Matroska copy keeps every packet" same_columns $RX/hevc_opus.mkv $O/rd_hevc.mkv 1-6
check "HEVC and Opus Matroska copy keeps the trims" same_trim $RX/hevc_opus.mkv $O/rd_hevc.mkv
check "HEVC Matroska copy decodes to the same frames" decodes_to $O/rd_hevc.mkv $HEVC_SUM
$A remux $RX/opus.ogg $O/rd_opus.mka
check "Ogg Opus copied to Matroska keeps every packet" same_columns $RX/opus.ogg $O/rd_opus.mka 1-6
check "Ogg Opus copied to Matroska keeps pre-skip and end trim" same_trim $RX/opus.ogg $O/rd_opus.mka
$A remux $RX/adts.aac $O/rd_adts.mka
check "ADTS AAC copied to Matroska keeps every packet" same_columns $RX/adts.aac $O/rd_adts.mka 1-6
$A remux $RX/vp9_opus.webm $O/rd_vp9.mp4 --video-only
check "WebM VP9 copied to MP4 keeps every packet" [ "$($A packets $RX/vp9_opus.webm --track 1 --raw | md5)" = "$($A packets $O/rd_vp9.mp4 --raw | md5)" ]
$A remux $RX/av1_opus.webm $O/rd_av1.mp4 --video-only
check "WebM AV1 copied to MP4 keeps every packet" [ "$($A packets $RX/av1_opus.webm --track 1 --raw | md5)" = "$($A packets $O/rd_av1.mp4 --raw | md5)" ]

echo "== readers: known signals and Apple's readers"
for f in u8.wav s24.wav s32.wav f32.wav f64.wav six.wav rf64.wav s8.aiff s24.aiff sowt.aifc fl32.aifc fl64.aifc s16be.mka s24le.mka f32le.mka prores_pcm.mkv; do
    check "$f holds the 440 Hz tone at one eighth of full scale" python3 tone.py tone $RX/$f 440
done
for f in $RX/*.wav $RX/*.aiff $RX/*.aifc; do check "$(basename $f) samples equal Apple's reader" python3 tone.py apple $f $O/rd_apple.wav; done
apple_packets() { [ "$(afinfo "$1" | sed -n 's/^audio packets: //p')" = "$($A packets "$1" | grep -cE '^-?[0-9]')" ]; }
apple_valid() {
    local trim=($($A packets "$1" | grep '^track' | cut -d' ' -f8,10))
    [ "$(afinfo "$1" | sed -n 's/^audio \([0-9]*\) valid frames.*/\1/p')" = "$((trim[1] - trim[0]))" ]
}
for f in cbr.mp3 vbr.mp3 plain.mp3 vbri.mp3 layer2.mp2 adts.aac fixed.flac opus.ogg; do check "$f has the packet count Apple finds" apple_packets $RX/$f; done
for f in cbr.mp3 opus.ogg; do check "$f trims to the length Apple finds" apple_valid $RX/$f; done

for f in $RX/*.ogg; do check "$(basename $f) page ends sit on their granule positions" python3 packets.py granules $f; done
has_tags() { $A probe "$1" | grep -qF "$2"; }
check "ID3v2.3 UTF-16 text is read" has_tags $RX/id3v23.mp3 "tags album=Älbum; artist=Plain Artist; date=2026; genre=Rock; title=Тест Ünï 音 🎵; track=3;"
check "ID3v2.4 UTF-8 text is read" has_tags $RX/id3v24.mp3 "tags album=Älbum; artist=Plain Artist; date=2026; genre=Rock; title=Тест Ünï 音 🎵; track=3;"
check "ID3v1 text is read" has_tags $RX/vbr.mp3 "tags album=Old Album; artist=Old Artist; date=2026; title=Old Title;"
check "Vorbis comments are read" has_tags $RX/opus.ogg "artist=Test Artist;"
check "WAV LIST INFO text is read" has_tags $RX/s16.wav "title=Test Title;"
check "AIFF text chunks are read" has_tags $RX/s16.aiff "title=Test Title;"
check "Matroska tags are read" has_tags $RX/h264_aac.mkv "title=Test Title;"

echo "== readers: broken files"
READER_FUZZ=60
[ -n "${QUICK:-}" ] && READER_FUZZ=12
refused() { ! $A probe "$1" > /dev/null 2> $O/rd_refused.txt && grep -q '^ffamp: ' $O/rd_refused.txt; }
: > $O/rd_empty.bin
printf 'This is a text file, not a media file.\n' > $O/rd_text.bin
head -c 3000 /dev/urandom > $O/rd_noise.bin
check "an empty file is refused with a message" refused $O/rd_empty.bin
check "a text file is refused with a message" refused $O/rd_text.bin
check "random bytes are refused with a message" refused $O/rd_noise.bin
check "cut, flipped, shortened and padded fixtures never crash" python3 fuzz.py $O/rd_fuzz.bin $READER_FUZZ $RX/*.wav $RX/*.aiff $RX/*.aifc $RX/*.mp3 $RX/*.mp2 $RX/*.aac $RX/*.flac $RX/*.ogg $RX/*.mkv $RX/*.webm $RX/*.mka

if ! have_ffmpeg; then
    skip "readers against ffprobe and ffmpeg"
    return 0
fi
echo "== readers: every packet against ffprobe and ffmpeg"
for f in $RX/*.wav $RX/*.aiff $RX/*.aifc; do check "$(basename $f) against ffmpeg" python3 packets.py pcm $f; done
for f in $RX/*.mp3 $RX/*.mp2 $RX/*.aac $RX/*.flac $RX/*.ogg; do
    [ "$(basename $f)" = vbri.mp3 ] && continue
    check "$(basename $f) against ffprobe" python3 packets.py $f
done
for f in $RX/*.mkv $RX/*.webm $RX/*.mka; do check "$(basename $f) against ffprobe" python3 packets.py $f 0.0015; done
payload_of() { "$FFMPEG" -nostdin -v error -i "$1" -map 0:$2 -c copy -f data - | md5; }
same_payload() { [ "$(payload_of "$1" $3)" = "$(payload_of "$2" $3)" ]; }
trims_like_ffmpeg() {
    local trim=($($A packets "$1" | grep '^track.*audio' | cut -d' ' -f8,10))
    [ "$(($($FFMPEG -nostdin -v error -i "$1" -vn -f s16le -ac 1 - | wc -c) / 2))" = "$((trim[1] - trim[0]))" ]
}
for f in cbr.mp3 vbr.mp3 opus.ogg vorbis.ogg hevc_opus.mkv mp3.mka; do check "$f trims to the length ffmpeg decodes" trims_like_ffmpeg $RX/$f; done

echo "== readers: longer files made now"
LONG="aevalsrc=0.3*sin(2*PI*(200+40*t)*t)|0.3*sin(2*PI*(900-20*t)*t):s=44100:d=20"
$F -i $O/h264_720.mp4 -c copy $O/rd_h264_720.mkv
$F -i $O/hevc_720.mp4 -c copy $O/rd_hevc_720.mkv
$F -f lavfi -i testsrc2=size=640x360:rate=24000/1001:duration=6 -f lavfi -i "$LONG" -c:v libx264 -preset veryfast -bf 3 -g 48 -pix_fmt yuv420p -c:a libopus -b:a 96k -shortest $O/rd_ntsc.mkv
$F -f lavfi -i testsrc2=size=320x180:rate=30:duration=6 -f lavfi -i "$LONG" -c:v libvpx-vp9 -b:v 400k -g 60 -cpu-used 8 -deadline realtime -c:a libopus -b:a 96k -shortest $O/rd_vp9.webm
$F -f lavfi -i "$LONG" -c:a libmp3lame -q:a 2 -metadata title="Long One" $O/rd_long.mp3
$F -f lavfi -i "$LONG" -c:a libmp3lame -b:a 192k -ar 32000 -ac 1 $O/rd_mono.mp3
$F -f lavfi -i "$LONG" -c:a aac -b:a 128k $O/rd_long.aac
$F -f lavfi -i "$LONG" -c:a flac -compression_level 8 $O/rd_long.flac
$F -f lavfi -i "$LONG" -c:a libopus -b:a 128k $O/rd_long.ogg
$F -f lavfi -i "$LONG" -c:a libopus -b:a 24k -frame_duration 60 -ac 1 $O/rd_slow.ogg
$F -f lavfi -i "$LONG" -c:a vorbis -strict -2 $O/rd_vorbis.ogg
$F -f lavfi -i "$LONG" -c:a flac -f ogg $O/rd_flac.ogg
$F -f lavfi -i "$LONG" -t 3 -ac 6 -ar 48000 -c:a pcm_s24le $O/rd_six.wav
for f in rd_h264_720.mkv rd_hevc_720.mkv rd_ntsc.mkv; do
    check "$f against ffprobe" python3 packets.py $O/$f 0.0015
    check "decode $f" python3 compare.py decode $O/$f
done
check "rd_vp9.webm against ffprobe" python3 packets.py $O/rd_vp9.webm 0.0015
for f in rd_long.mp3 rd_mono.mp3 rd_long.aac rd_long.flac rd_long.ogg rd_slow.ogg rd_vorbis.ogg rd_flac.ogg; do check "$f against ffprobe" python3 packets.py $O/$f; done
check "rd_six.wav against ffmpeg" python3 packets.py pcm $O/rd_six.wav
for f in rd_long.mp3 rd_long.ogg rd_slow.ogg rd_vorbis.ogg; do check "$f trims to the length ffmpeg decodes" trims_like_ffmpeg $O/$f; done
$A remux $O/rd_ntsc.mkv $O/rd_ntsc_copy.mkv
check "copy of rd_ntsc.mkv has no ffprobe or decode errors" clean $O/rd_ntsc_copy.mkv
check "copy of rd_ntsc.mkv keeps every video packet" same_payload $O/rd_ntsc.mkv $O/rd_ntsc_copy.mkv 0
check "copy of rd_ntsc.mkv keeps every sound packet" same_payload $O/rd_ntsc.mkv $O/rd_ntsc_copy.mkv 1
check "copy of rd_ntsc.mkv against ffprobe" python3 packets.py $O/rd_ntsc_copy.mkv 0.0015
check "copy of rd_ntsc.mkv decodes frame-exact" python3 compare.py decode $O/rd_ntsc_copy.mkv
$A remux $O/rd_h264_720.mkv $O/rd_h264_720_copy.mp4 --video-only
check "MP4 copy of rd_h264_720.mkv has no ffprobe or decode errors" clean $O/rd_h264_720_copy.mp4
check "MP4 copy of rd_h264_720.mkv decodes frame-exact" python3 compare.py decode $O/rd_h264_720_copy.mp4
$A remux $O/rd_long.ogg $O/rd_long_copy.mka
check "Matroska copy of rd_long.ogg has no ffprobe or decode errors" clean $O/rd_long_copy.mka
check "Matroska copy of rd_long.ogg keeps every packet" same_payload $O/rd_long.ogg $O/rd_long_copy.mka 0
check "Matroska copy of rd_long.ogg trims to the length ffmpeg decodes" trims_like_ffmpeg $O/rd_long_copy.mka
rm -f $O/rd_*
