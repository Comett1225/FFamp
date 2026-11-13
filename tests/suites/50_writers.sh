echo "== writers: sound files"
clang -fobjc-arc -O1 -w audiocheck.m -o ../build/audiocheck -framework AudioToolbox -framework Foundation || failed=1
W=writers_check.py
WF=fixtures/writers
WO=$O/writers
mkdir -p $WO
apple_frames() { [ "$(../build/audiocheck "$1" s16 | sed 's/.*frames //')" = "$2" ]; }
apple_same() { ../build/audiocheck "$1" "$2" raw > $WO/apple.raw && python3 $W same $WO/apple.raw "$3" "${2#s}"; }
apple_lossy() { ../build/audiocheck "$1" s16 raw > $WO/apple.raw && python3 $W lossy $WO/apple.raw "$2" "$3"; }
av_opens() { ../build/avcheck "$1" > /dev/null; }
av_frames() { ../build/avcheck "$1" | grep -q "playable 1 .* audio_frames $2 "; }
ff_same() { $F -i "$1" -f "$2" $WO/ff.raw && python3 $W same $WO/ff.raw "$3" "$4"; }
ff_lossy() { $F -i "$1" -f s16le $WO/ff.raw && python3 $W lossy $WO/ff.raw "$2" "$3"; }
encodes() { $A aencode "$1" --pcm "$2" --rate "$3" --channels "$4" --format "$5" "${@:6}"; }
refuses() { ! "$@" 2> /dev/null; }
stream_bytes() { "$FFMPEG" -nostdin -v error -i "$1" -map "0:$2" -c copy -f data - | md5; }
same_bytes() { [ "$(stream_bytes "$1" 0)" = "$(stream_bytes "$2" 0)" ] && [ "$(stream_bytes "$1" 1)" = "$(stream_bytes "$2" 1)" ]; }
raw_name() { case $1 in 16) echo s16le ;; 24) echo s24le ;; *) echo f32le ;; esac; }

python3 $W signal $WO/music.f32 music 48000 2 143223
FRAMES=143223
for kind in 16 24 f; do
    out=$WO/music_$kind.wav
    check "WAV $kind encodes" encodes $out $WO/music.f32 48000 2 wav$kind
    check "WAV $kind structure and samples are exact" python3 $W wav $out $WO/music.f32 48000 2 $kind
    check "WAV $kind opens in AudioToolbox with $FRAMES frames" apple_frames $out $FRAMES
    check "WAV $kind opens in AVFoundation with $FRAMES frames" av_frames $out $FRAMES
    if have_ffmpeg; then
        check "WAV $kind is clean in FFmpeg" clean $out
        check "WAV $kind decodes bit-exact in FFmpeg" ff_same $out $(raw_name $kind) $WO/music.f32 $kind
    else skip "WAV $kind against FFmpeg"; fi
done
check "WAV 16 samples are exact through AudioToolbox" apple_same $WO/music_16.wav s16 $WO/music.f32
check "WAV 24 samples are exact through AudioToolbox" apple_same $WO/music_24.wav s24 $WO/music.f32
check "RF64 encodes when forced" encodes $WO/wide.wav $WO/music.f32 48000 2 wav24 --rf64
check "RF64 structure and samples are exact" python3 $W wav $WO/wide.wav $WO/music.f32 48000 2 24 wide
check "RF64 float encodes when forced" encodes $WO/wide_f.wav $WO/music.f32 48000 2 wavf --rf64
check "RF64 float structure and samples are exact" python3 $W wav $WO/wide_f.wav $WO/music.f32 48000 2 f wide
have_ffmpeg && check "RF64 decodes bit-exact in FFmpeg" ff_same $WO/wide.wav s24le $WO/music.f32 24
python3 $W signal $WO/six.f32 tone 44100 6 5001
python3 $W signal $WO/one.f32 tone 22050 1 3333
for layout in "six 44100 6" "one 22050 1"; do
    set -- $layout
    for kind in 16 24 f; do
        check "WAV $kind with $3 channel(s) encodes" encodes $WO/$1_$kind.wav $WO/$1.f32 $2 $3 wav$kind
        check "WAV $kind with $3 channel(s) is exact" python3 $W wav $WO/$1_$kind.wav $WO/$1.f32 $2 $3 $kind
        have_ffmpeg && check "WAV $kind with $3 channel(s) decodes bit-exact in FFmpeg" ff_same $WO/$1_$kind.wav $(raw_name $kind) $WO/$1.f32 $kind
    done
done
check "16-bit samples arrive unchanged from a 16-bit pipe" [ "$(python3 $W payload $WF/pcm_s16.pkts | $A aencode $WO/pipe.wav --pcm - --rate 48000 --channels 2 --format wav16 --sample s16 && tail -c 36000 $WO/pipe.wav | md5)" = "$(python3 $W payload $WF/pcm_s16.pkts | md5)" ]

check "a stereo pipe is mixed down to mono" [ "$($A aencode $WO/down.wav --pcm $WO/music.f32 --rate 48000 --channels 1 --source-channels 2 --format wavf && ../build/audiocheck $WO/down.wav s16)" = "rate 48000 channels 1 frames $FRAMES" ]

echo "== writers: FLAC from our own encoder"
flac_case() {
    local name=$1 kind=$2 rate=$3 channels=$4 frames=$5 bits=$6
    local out=$WO/$name.flac
    python3 $W signal $WO/$name.f32 $kind $rate $channels $frames
    check "FLAC $name encodes" encodes $out $WO/$name.f32 $rate $channels flac${bits/16/}
    check "FLAC $name structure, checksums, MD5 and seek table" python3 $W flac $out $WO/$name.f32 $rate $channels $bits
    check "FLAC $name decodes bit-exact in AudioToolbox" apple_same $out s$bits $WO/$name.f32
    if have_ffmpeg; then
        check "FLAC $name is clean in FFmpeg" clean $out
        check "FLAC $name decodes bit-exact in FFmpeg" ff_same $out s${bits}le $WO/$name.f32 $bits
    else skip "FLAC $name against FFmpeg"; fi
}
flac_case music16 music 48000 2 143223 16
flac_case music24 music 48000 2 143223 24
flac_case music441 music 44100 2 100000 16
flac_case mono music 44100 1 50000 16
flac_case six music 48000 6 30000 16
flac_case high music 96000 2 60000 24
flac_case oddrate tone 12345 2 20000 16
flac_case bigrate tone 352800 2 20000 24
flac_case noise noise 48000 2 30000 16
flac_case noise24 noise 48000 2 30000 24
flac_case silence silence 48000 2 30000 16
flac_case steady steady 48000 2 30000 24
flac_case coarse24 coarse 48000 2 30000 24
flac_case exact tone 48000 2 4608 16
flac_case plusone tone 48000 2 4609 16
flac_case tiny tone 48000 2 7 16
flac_case single tone 48000 1 1 16
flac_case long music 48000 2 2000000 16
rm -f $WO/long.f32
if have_ffmpeg; then
    for name in music16 music24 music441 mono six high long; do
        $F -i $WO/$name.flac -c:a flac $WO/ff_$name.flac
        ours=$(python3 $W flacaudio $WO/$name.flac)
        theirs=$(python3 $W flacaudio $WO/ff_$name.flac)
        echo "  FLAC frame bytes $name: ours $ours, FFmpeg default $theirs ($(( (ours - theirs) * 1000 / theirs )) per mille)"
        check "FLAC $name is within 5 % of FFmpeg's size" [ $(( ours * 100 )) -le $(( theirs * 105 )) ]
    done
fi

echo "== writers: M4A, Ogg Opus, MP3"
check "M4A encodes" encodes $WO/music.m4a $WO/music.f32 48000 2 aac --bitrate 192000
check "M4A has the exact length in AudioToolbox" apple_frames $WO/music.m4a $FRAMES
check "M4A has the exact length in AVFoundation" av_frames $WO/music.m4a $FRAMES
check "M4A sounds like the source (AudioToolbox)" apple_lossy $WO/music.m4a $WO/music.f32 2
check "M4A carries the M4A brand" [ "$(head -c 12 $WO/music.m4a | tail -c 4)" = "M4A " ]
python3 $W signal $WO/mono441.f32 music 44100 1 99999
check "M4A mono 44.1 kHz encodes" encodes $WO/mono.m4a $WO/mono441.f32 44100 1 aac --bitrate 96000
check "M4A mono 44.1 kHz has the exact length in AudioToolbox" apple_frames $WO/mono.m4a 99999
check "M4A mono 44.1 kHz sounds like the source" apple_lossy $WO/mono.m4a $WO/mono441.f32 1
if $A aencode $WO/music.ogg --pcm $WO/music.f32 --rate 48000 --channels 2 --format opus --bitrate 128000 2> /dev/null; then
    check "Ogg Opus pages, checksums, headers and granules" python3 $W ogg $WO/music.ogg $FRAMES 2
    check "Ogg Opus has the exact length in AudioToolbox" apple_frames $WO/music.ogg $FRAMES
    check "Ogg Opus sounds like the source (AudioToolbox)" apple_lossy $WO/music.ogg $WO/music.f32 2
    python3 $W signal $WO/short.f32 tone 48000 1 500
    check "Ogg Opus short mono encodes" encodes $WO/short.ogg $WO/short.f32 48000 1 opus --bitrate 64000
    check "Ogg Opus short mono structure" python3 $W ogg $WO/short.ogg 500 1
    check "Ogg Opus refuses other rates" refuses encodes $WO/bad.ogg $WO/mono441.f32 44100 1 opus
    if have_ffmpeg; then
        check "Ogg Opus is clean in FFmpeg" clean $WO/music.ogg
        check "Ogg Opus has the exact length and sound in FFmpeg" ff_lossy $WO/music.ogg $WO/music.f32 2
    else skip "Ogg Opus against FFmpeg"; fi
else
    skip "Ogg Opus (libopus not found when building)"
fi
if $A aencode $WO/music.mp3 --pcm $WO/music.f32 --rate 48000 --channels 2 --format mp3 --bitrate 192000 2> /dev/null; then
    check "MP3 has the exact length in AudioToolbox" apple_frames $WO/music.mp3 $FRAMES
    check "MP3 has the exact length in AVFoundation" av_frames $WO/music.mp3 $FRAMES
    check "MP3 sounds like the source (AudioToolbox)" apple_lossy $WO/music.mp3 $WO/music.f32 2
    check "MP3 starts with an Info frame" grep -q "Info" <(head -c 64 $WO/music.mp3)
    check "MP3 mono 44.1 kHz encodes" encodes $WO/mono.mp3 $WO/mono441.f32 44100 1 mp3 --bitrate 128000
    check "MP3 mono 44.1 kHz has the exact length in AudioToolbox" apple_frames $WO/mono.mp3 99999
    if have_ffmpeg; then
        check "MP3 is clean in FFmpeg" clean $WO/music.mp3
        check "MP3 has the exact length and sound in FFmpeg" ff_lossy $WO/music.mp3 $WO/music.f32 2
        check "MP3 is constant bitrate at 192 kb/s" [ "$("$FFPROBE" -v error -show_entries stream=bit_rate -of csv=p=0 $WO/music.mp3)" = 192000 ]
    else skip "MP3 against FFmpeg"; fi
else
    skip "MP3 (LAME not found when building)"
fi
if have_ffmpeg; then
    check "M4A is clean in FFmpeg" clean $WO/music.m4a
    check "M4A has the exact length and sound in FFmpeg" ff_lossy $WO/music.m4a $WO/music.f32 2
    check "M4A mono has the exact length and sound in FFmpeg" ff_lossy $WO/mono.m4a $WO/mono441.f32 1
else skip "M4A against FFmpeg"; fi

echo "== writers: container entries for every codec"
packs() { $A pack "$@" 2> /dev/null; }
entry_case() {
    local name=$1 file=$2 brand=$3 kind=mp4
    local out=$WO/${name}.$file
    case $file in mkv) kind=mkv; brand=matroska ;; webm) kind=mkv; brand=webm ;; esac
    check "$name into $file packs" packs $out "${@:4}"
    check "$name in $file: structure and every packet (own parser)" python3 $W $kind $out "$brand" "${@:4}"
    if have_ffmpeg; then
        check "$name in $file is clean in FFmpeg" clean $out
        check "$name in $file: FFmpeg reads the same packets" python3 $W copied $out "${@:4}"
    else skip "$name in $file against FFmpeg"; fi
}
for name in h264 hevc av1 prores aac alac pcm_s16 pcm_s24 pcm_f32 pcm_s16be pcm_f64_6ch; do
    entry_case $name mp4 isom $WF/$name.pkts
    entry_case $name mov "qt  " $WF/$name.pkts
done
for name in vp9 vp8 opus flac; do
    entry_case $name mp4 isom $WF/$name.pkts
    check "$name is refused in QuickTime" refuses $A pack $WO/no_$name.mov $WF/$name.pkts
    check "refused $name leaves no file behind" [ ! -e $WO/no_$name.mov ] && [ ! -e $WO/no_$name.mov.part ]
done
for name in h264 hevc av1 vp9 vp8 aac opus flac alac pcm_s16 pcm_s24 pcm_f32 pcm_s16be pcm_f64_6ch; do entry_case $name mkv "" $WF/$name.pkts; done
for name in av1 vp9 vp8 opus; do entry_case $name webm "" $WF/$name.pkts; done
for name in h264 hevc aac flac pcm_s16 prores; do check "$name is refused in WebM" refuses $A pack $WO/no_$name.webm $WF/$name.pkts; done
entry_case h264_aac mp4 isom $WF/h264.pkts $WF/aac.pkts
entry_case prores_pcm mov "qt  " $WF/prores.pkts $WF/pcm_s24.pkts
entry_case hevc_alac mov "qt  " $WF/hevc.pkts $WF/alac.pkts
entry_case av1_opus mp4 isom $WF/av1.pkts $WF/opus.pkts
entry_case av1_opus mkv "" $WF/av1.pkts $WF/opus.pkts
entry_case vp9_opus webm "" $WF/vp9.pkts $WF/opus.pkts
entry_case h264_flac mkv "" $WF/h264.pkts $WF/flac.pkts
if packs $WO/opus.ogg $WF/opus.pkts; then
    check "Opus packets into Ogg: pages, checksums, granules" python3 $W ogg $WO/opus.ogg 29448 2
    have_ffmpeg && check "Opus packets in Ogg are clean in FFmpeg" clean $WO/opus.ogg
    have_ffmpeg && check "Opus packets in Ogg: FFmpeg reads the same packets" python3 $W copied $WO/opus.ogg $WF/opus.pkts
else
    check "Opus packets into Ogg pack" false
fi
check "Ogg refuses anything but Opus" refuses $A pack $WO/no_flac.ogg $WF/flac.pkts
survives() { "$@" > /dev/null 2>&1; [ $? -lt 128 ]; }
cut_files_survive() {
    local name size
    for name in opus flac alac av1 vp9 pcm_s24 h264; do
        size=$(stat -f %z $WF/$name.pkts)
        for at in 9 30 57 58 60 80 120 $((size / 3)) $((size / 2)) $((size - 1)); do
            head -c $at $WF/$name.pkts > $WO/cut.pkts
            for file in mp4 mov mkv webm ogg m4a; do survives $A pack $WO/cut.$file $WO/cut.pkts || return 1; done
        done
    done
}
check "cut-off packet files never crash a writer" cut_files_survive
check "M4A takes sound only" refuses $A pack $WO/no_video.m4a $WF/h264.pkts
check "M4A from packets packs" packs $WO/alac.m4a $WF/alac.pkts
check "M4A from packets: structure and every packet" python3 $W mp4 $WO/alac.m4a "M4A " $WF/alac.pkts

apple_payload() { ../build/audiocheck "$1" "$2" raw | cmp -s - <(python3 $W payload "${@:3}"); }
for file in mp4 mov; do
    for name in h264 hevc av1 prores; do check "$name in $file opens in AVFoundation" av_opens $WO/$name.$file; done
    check "PCM 16 in $file plays the same samples (AudioToolbox)" apple_payload $WO/pcm_s16.$file s16 $WF/pcm_s16.pkts
    check "PCM 24 in $file plays the same samples (AudioToolbox)" apple_payload $WO/pcm_s24.$file s24 $WF/pcm_s24.pkts
    check "PCM float in $file plays the same samples (AudioToolbox)" apple_payload $WO/pcm_f32.$file f32 $WF/pcm_f32.pkts
    check "PCM 16 big-endian in $file plays the same samples (AudioToolbox)" apple_payload $WO/pcm_s16be.$file s16 $WF/pcm_s16be.pkts swap
    check "PCM float 64 with 6 channels in $file has 2000 frames in AVFoundation" av_frames $WO/pcm_f64_6ch.$file 2000
    check "ALAC in $file has 28800 frames in AVFoundation" av_frames $WO/alac.$file 28800
    check "AAC in $file opens in AVFoundation" av_opens $WO/aac.$file
done
check "FLAC in MP4 has 28800 frames in AVFoundation" av_frames $WO/flac.mp4 28800
check "Opus in MP4 opens in AVFoundation" av_opens $WO/opus.mp4
check "ProRes with PCM in MOV opens in AVFoundation with 9000 sound frames" av_frames $WO/prores_pcm.mov 9000
check "HEVC with ALAC in MOV opens in AVFoundation" av_opens $WO/hevc_alac.mov
$A transcode fixtures/video/h264_270p.mp4 $WO/transcode.mov --codec hevc --bitrate 1000000 2> /dev/null
check "transcode into MOV opens in AVFoundation" av_opens $WO/transcode.mov
have_ffmpeg && check "transcode into MOV is clean in FFmpeg" clean $WO/transcode.mov
for f in h264_270p.mp4 hevc_270p.mp4; do
    for file in mov mkv; do
        $A remux fixtures/video/$f $WO/remux_${f%.mp4}.$file
        if [ $file = mov ]; then check "remux $f into MOV opens in AVFoundation" av_opens $WO/remux_${f%.mp4}.mov; fi
        have_ffmpeg && check "remux $f into $file is clean in FFmpeg" clean $WO/remux_${f%.mp4}.$file
        have_ffmpeg && check "remux $f into $file keeps every packet" same_bytes fixtures/video/$f $WO/remux_${f%.mp4}.$file
    done
    check "remux $f into MOV decodes to the same frames" [ "$($A decode $WO/remux_${f%.mp4}.mov | md5)" = "$($A decode fixtures/video/$f | md5)" ]
done
