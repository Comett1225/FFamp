echo "== installed: Vorbis through libvorbis, no FFmpeg needed"
IA=fixtures/audio
IP="python3 audio.py"
II="python3 installed.py"
IV=/Applications/VLC.app/Contents/MacOS/VLC
check "vorbis_stereo_44k.ogg: rate, channels, length to the sample, no offset from its source signal" $IP truth $IA/vorbis_stereo_44k.ogg 44100 2 1.5 25 0
check "vorbis_51_48k.ogg: every channel in its place against its source signal" $IP truth $IA/vorbis_51_48k.ogg 48000 6 1.5 15 0
for f in $(cut -d' ' -f3 $IA/decoded_vorbis.md5); do check "$f decodes to the checked checksum" $IP checksum $IA/$f $IA/decoded_vorbis.md5; done
for f in vorbis_stereo_44k.ogg vorbis_51_48k.ogg vorbis_mono_44k.mka vorbis_stereo_44k.webm; do check "random reads $f" $IP chunks adecode $IA/$f 30 3; done
check "Vorbis 5.1 as 44.1 kHz stereo: random reads" $IP chunks pcm $IA/vorbis_51_48k.ogg 30 4 --rate 44100 --channels 2
check "Vorbis from WebM to AAC in M4A" $A extract-audio $IA/vorbis_stereo_44k.webm $O/in_vorbis.m4a --aac --bitrate 128000 > /dev/null
check "the AAC made from Vorbis plays in AVFoundation" ../build/avcheck $O/in_vorbis.m4a > /dev/null
if have_ffmpeg; then
    for f in vorbis_stereo_44k.ogg vorbis_51_48k.ogg vorbis_mono_44k.mka vorbis_stereo_44k.webm; do check "$f against ffmpeg: same length, no offset, over 100 dB" $IP against $IA/$f 100 0 0; done
fi

echo "== installed: fast sound paths stay exact"
$IP signal 44100 2 20 $O/in_long.wav
in_pcm() { $A pcm $O/in_long.wav --rate 48000 --channels 2 "$@" 2> /dev/null | md5; }
check "threaded PcmReader equals the streaming resampler to the bit" [ "$(in_pcm)" = "$($A adecode $O/in_long.wav 2> /dev/null | $A resample --from 44100 --to 48000 --channels 2 | md5)" ]
check "PcmReader gives the same bits in 777, 8192 and 300000 frame reads" [ "$(in_pcm --chunk 777)$(in_pcm --chunk 300000)" = "$(in_pcm)$(in_pcm)" ]
$A pcm $O/in_long.wav --rate 44100 --channels 2 --format s16le > $O/in_long.s16 2> /dev/null
$A pcm $O/in_long.wav --rate 48000 --channels 6 > $O/in_six.f32 2> /dev/null
$A aencode $O/in_long.flac --pcm $O/in_long.s16 --sample s16 --rate 44100 --channels 2 --format flac
$A aencode $O/in_again.flac --pcm $O/in_long.s16 --sample s16 --rate 44100 --channels 2 --format flac
$A aencode $O/in_deep.flac --pcm $O/in_long.s16 --sample s16 --rate 44100 --channels 2 --format flac24
$A aencode $O/in_mono.flac --pcm $O/in_long.s16 --sample s16 --source-channels 2 --rate 44100 --channels 1 --format flac
$A aencode $O/in_six.flac --pcm $O/in_six.f32 --rate 48000 --channels 6 --format flac
check "FLAC frames made on several threads are the same in every run" cmp -s $O/in_long.flac $O/in_again.flac
check "our FLAC decodes back to the samples it was given" [ "$($A pcm $O/in_long.flac --rate 44100 --channels 2 --format s16le 2> /dev/null | md5)" = "$(md5 < $O/in_long.s16)" ]
if command -v flac > /dev/null; then
    check "the flac tool decodes our FLAC to the samples it was given" [ "$(flac -d -s -c --force-raw-format --endian=little --sign=signed $O/in_long.flac | md5)" = "$(md5 < $O/in_long.s16)" ]
    for f in $(find $O -name '*.flac' | sort); do check "flac -t ${f#$O/}" flac -t -s $f; done
else
    skip "flac tool not installed: flac -t on every FLAC file"
fi
if have_ffmpeg; then
    $F -f s16le -ar 44100 -ac 2 -i $O/in_long.s16 -c:a flac $O/in_judge.flac
    check "our FLAC is no larger than ffmpeg's" [ "$(stat -f %z $O/in_long.flac)" -le "$(stat -f %z $O/in_judge.flac)" ]
    rm -f $O/in_judge.flac
fi

echo "== installed: APV through liboapv"
$A decode fixtures/video/h264_270p.mp4 > $O/in_src.yuv
$A decode fixtures/video/h264_270p.mp4 --bgra > $O/in_src.bgra
$A pixels --size 480x270 --video $O/in_src.bgra --out $O/in_ref.yuv --no-system
in_judge=""
have_ffmpeg && in_judge=ffmpeg
$A encode $O/in_apv.mp4 --video $O/in_src.yuv --size 480x270 --fps 30 --codec apv --bitrate 20000000 2> /dev/null
check "APV from I420: 10-bit 4:2:2, PSNR over 50 dB${in_judge:+, ffmpeg decodes the same bits}" $II apv $O/in_apv.mp4 $O/in_src.yuv 480 270 50 $in_judge
check "APV in MP4: apv1 track, every frame a key frame" [ "$($A probe $O/in_apv.mp4 | grep -c 'video apv1 480x270 rotation 0 frames 60 .* keyframes 60 ')" = 1 ]
check "APV probe: codec, pixel format and colour" [ "$($A probe --json $O/in_apv.mp4 | grep -c '"codec_name":"apv".*"pix_fmt":"yuv422p10le","color_range":"tv","color_space":"bt709"')" = 1 ]
check "APV seeks to exact frames" python3 comparex.py selfseek $O/in_apv.mp4 20 3
check "damaged APV never crashes" python3 comparex.py fuzz $O/in_apv.mp4 30 1
$A encode $O/in_apv_bgra.mp4 --video $O/in_src.bgra --size 480x270 --fps 30 --codec apv --format bgra --bitrate 20000000 2> /dev/null
check "APV from BGRA: PSNR over 50 dB" $II apv $O/in_apv_bgra.mp4 $O/in_ref.yuv 480 270 50 $in_judge
$A transcode fixtures/video/hevc10_270p.mp4 $O/in_apv_hevc.mp4 --codec apv --bitrate 10000000 2> /dev/null
check "decoded 10-bit HEVC frames go into APV" [ "$($A decode $O/in_apv_hevc.mp4 | wc -c | tr -d ' ')" = $((480 * 270 * 4 * 48)) ]
$A compress fixtures/video/h264_270p.mp4 $O/in_apv_tool.mp4 --codec apv > /dev/null 2>&1
check "the compress tool writes APV with sound kept" [ "$($A probe $O/in_apv_tool.mp4 | grep -c 'video apv1\|audio mp4a')" = 2 ]
in_refused() { ! "$@" 2> /dev/null; }
check "APV is refused in Matroska and leaves no file" in_refused $A encode $O/in_apv.mkv --video $O/in_src.yuv --size 480x270 --fps 30 --codec apv
check "APV with an odd width is refused" in_refused $A encode $O/in_apv_odd.mp4 --video $O/in_src.yuv --size 479x270 --fps 30 --codec apv
if have_ffmpeg; then
    check "APV file is clean in ffmpeg" clean $O/in_apv.mp4
    check "ffprobe sees APV 4:2:2 10-bit, BT.709 limited" [ "$("$FFPROBE" -v error -show_entries stream=codec_name,pix_fmt,color_space,color_range -of csv=p=0 $O/in_apv.mp4)" = "apv,yuv422p10le,tv,bt709" ]
fi
rm -f $O/in_src.bgra $O/in_ref.yuv $O/in_src.yuv

echo "== installed: transport stream with empty stuffing packets (YouTube HLS)"
$II hollow fixtures/probe/h264_aac.ts $O/in_hollow.ts > /dev/null
check "empty packets cost no picture" [ "$($A decode $O/in_hollow.ts | md5)" = "$($A decode fixtures/probe/h264_aac.ts | md5)" ]
check "empty packets cost no sound" [ "$($A dump $O/in_hollow.ts --track 1 | md5)" = "$($A dump fixtures/probe/h264_aac.ts --track 1 | md5)" ]

echo "== installed: outside judges on the files every suite wrote"
if command -v mkvmerge > /dev/null; then
    for f in $(find $O -name '*.mkv' -o -name '*.mka' -o -name '*.webm' | sort); do check "mkvmerge finds no error or warning in ${f#$O/}" $II mkv $f; done
else
    skip "mkvtoolnix not installed: mkvmerge on every Matroska and WebM file"
fi
if [ -x $IV ]; then
    in_vlc() { [ -f "$1" ] || return 0; check "VLC opens ${1#$O/} without a window" $II vlc "$@"; }
    in_vlc $O/in_long.flac flacsys audio
    in_vlc $IA/vorbis_stereo_44k.webm mkv audio
    in_vlc $O/in_vorbis.m4a mp4 audio
    in_vlc $O/writers/h264_flac.mkv mkv audio video
    in_vlc $O/writers/vp9_opus.webm mkv audio video
    in_vlc $O/writers/h264_aac.mp4 mp4 audio video
    in_vlc $O/writers/opus.ogg ogg audio
    in_vlc $O/writers/music_16.wav wav audio
else
    skip "VLC not installed: opening our files in VLC"
fi
