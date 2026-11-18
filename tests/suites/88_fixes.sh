echo "== fixes: one check per reviewed finding, made and read back by ffamp itself (no FFmpeg), then FFmpeg as the judge when present"
FX=$O/fixes
rm -rf $FX
mkdir -p $FX
for name in $($A fixcheck); do check "fixes: $name" $A fixcheck $name $FX fixtures; done
fx_named() {
    mkdir -p "$FX/$1" && cp fixtures/audio/aac_stereo_44k.m4a "$FX/$1/clip $1.m4a" && cp fixtures/audio/mp3_stereo_44k.mp4 "$FX/$1/clip $1.mp4" || return 1
    $A probe --json "$FX/$1/clip $1.m4a" | grep -q '"codec_name":"aac"' && $A extract-audio "$FX/$1/clip $1.mp4" "$FX/$1/sound $1.mp3" > /dev/null &&
        $A extract-audio "$FX/$1/clip $1.m4a" "$FX/$1/sound $1.m4a" > /dev/null && $A adecode "$FX/$1/sound $1.mp3" > /dev/null 2>&1 && $A adecode "$FX/$1/sound $1.m4a" > /dev/null 2>&1
}
for name in Café Привет 動画; do check "fixes: a folder and file named $name open, probe and extract-audio into it" fx_named $name; done
check "fixes: compress --fps 29.97 encodes at 30000/1001, not 29" eval '$A compress fixtures/tools/clip.mp4 $FX/fps.mp4 --fps 29.97 --audio none > /dev/null 2>&1 && $A probe $FX/fps.mp4 | grep -q "timescale 30000 "'
kept_after() { printf keep > "$1" && ! "${@:2}" > /dev/null 2>&1 && [ "$(cat "$1")" = keep ] && [ ! -e "$1.part" ]; }
head -c 1000 /dev/zero > $FX/short.raw
check "fixes: frames-in with an incomplete frame keeps the file already at the output" kept_after $FX/kept_in.mp4 $A frames-in $FX/kept_in.mp4 --size 64x64 --fps 30 --in $FX/short.raw
check "fixes: speech from a missing file keeps the file already at the output" kept_after $FX/kept_speech.wav $A speech $FX/no_such_voice.wav $FX/kept_speech.wav
if have_ffmpeg; then
    decoded_bytes() { [ "$($FFMPEG -nostdin -v error -i "$1" -f s16le - | wc -c | tr -d ' ')" = "$2" ]; }
    stated_length() { [ "$($FFPROBE -v error -show_entries format=duration -of csv=p=0 "$1" 2> /dev/null)" = "$2" ]; }
    loudness_order() { python3 -c 'import sys, numpy; d = numpy.frombuffer(sys.stdin.buffer.read(), "<f4"); n = int(sys.argv[1]); f = d[:len(d) // n * n].reshape(-1, n); print(*numpy.argsort(numpy.abs(f[len(f) // 4:len(f) * 3 // 4]).max(axis=0)))' "$1"; }
    same_levels() { [ "$($A adecode "$1" 2> /dev/null | loudness_order "$2")" = "$($FFMPEG -nostdin -v error -i "$1" -f f32le - | loudness_order "$2")" ]; }
    check "fixes: FFmpeg decodes nothing from the empty AAC file" decoded_bytes $FX/empty.m4a 0
    check "fixes: FFmpeg decodes nothing from the empty Opus file" decoded_bytes $FX/empty.opus 0
    check "fixes: FFmpeg decodes 100 frames from the 100-frame AAC file" decoded_bytes $FX/short.m4a 400
    first_sample() { [ "$($FFMPEG -nostdin -v error -i "$1" -f s16le - | head -c 2 | od -An -tx1 | tr -d ' \n')" = 0064 ]; }
    for f in signed8 unsigned8; do for ext in mp4 mov mkv; do check "fixes: FFmpeg reads the sign of 8-bit PCM in $f.$ext" first_sample $FX/$f.$ext; done; done
    check "fixes: FFmpeg decodes 100 frames from the 100-frame Opus file" decoded_bytes $FX/short.opus 400
    check "fixes: FFmpeg reads the 13 hour length of the long MP4" stated_length $FX/long.m4a 47722.858844
    check "fixes: FFmpeg reads the late trim of the PCM track in MP4" stated_length $FX/trimmed.mp4 1.000000
    check "fixes: FFmpeg reads the late trim of the PCM track in Matroska" stated_length $FX/late.mka 0.052083
    check "fixes: 7.1 AAC channels come out in FFmpeg's order" same_levels fixtures/fixes/aac_71.m4a 8
    if on_mac; then
        skip "fixes: 6.1 ALAC and AAC in FFmpeg's order (the Mac keeps AudioToolbox's WAVE 6.1 order)"
    else
        check "fixes: 6.1 ALAC channels come out in FFmpeg's order" same_levels fixtures/fixes/alac_61.m4a 7
        check "fixes: 6.1 AAC channels come out in FFmpeg's order" same_levels fixtures/fixes/aac_61.m4a 7
    fi
    $F -f lavfi -i testsrc2=s=640x360:r=30:d=12 -pix_fmt rgb0 -f rawvideo $FX/seek.rgb
    $F -f lavfi -i sine=r=48000:d=12 -ac 2 -f s16le $FX/seek.pcm
    sound_after_seek() {
        local out=$FX/seek_$1_$2.$3 at bytes
        $A encode $out --video $FX/seek.rgb --format rgb0 --size 640x360 --fps 30 --codec $1 --bitrate 2000000 --audio $FX/seek.pcm --rate 48000 --channels 2 --acodec $2 > /dev/null 2>&1 || return 1
        for at in 3 6 9; do
            bytes=$($FFMPEG -nostdin -v error -ss $at -i $out -t 2 -vn -f s16le - | wc -c | tr -d ' ')
            [ "$bytes" -ge 360000 ] || return 1
        done
    }
    check "fixes: an AV1 + Opus WebM export has its sound after a seek (FFmpeg reads 2 s from 3, 6 and 9 s)" sound_after_seek av1 opus webm
    check "fixes: an AV1 + AAC Matroska export has its sound after a seek" sound_after_seek av1 aac mkv
    check "fixes: an H.264 + AAC Matroska export has its sound after a seek" sound_after_seek h264 aac mkv
    same_rate() { [ "$($FFPROBE -v error -select_streams v:0 -show_entries stream=r_frame_rate,avg_frame_rate -of csv=p=0 "$1")" = "$2" ]; }
    check "fixes: the H.264 Matroska export states 30 fps (DefaultDuration)" same_rate $FX/seek_h264_aac.mkv 30/1,30/1
    $A rewrap fixtures/tools/clip.mp4 $FX/rate.mkv > /dev/null 2>&1
    check "fixes: an MP4 rewrapped into Matroska states 30 fps" same_rate $FX/rate.mkv 30/1,30/1
    trimmed_codecs() {
        $A trim "$1" "$2" --start 0.2 --end 0.7 > /dev/null 2>&1 && clean "$2" && [ "$($FFPROBE -v error -show_entries stream=codec_name -of default=nw=1:nk=1 "$2" | tr -d '\r' | tr '\n' ' ')" = "$3" ]
    }
    check "fixes: fast trim of MPEG-4 + MP3 AVI into MP4" trimmed_codecs fixtures/formats/avi_mpeg4_mp3.avi $FX/t_mp3.mp4 "mpeg4 mp3 "
    check "fixes: fast trim of Motion JPEG + PCM AVI into MP4" trimmed_codecs fixtures/formats/avi_mjpeg_pcm.avi $FX/t_mjpeg.mp4 "mjpeg pcm_s16le "
    check "fixes: fast trim of H.264 + AC-3 into MP4" trimmed_codecs fixtures/probe/h264_ac3.ts $FX/t_ac3.mp4 "h264 ac3 "
    check "fixes: fast trim of H.264 + E-AC-3 into MP4" trimmed_codecs fixtures/probe/h264_eac3.ts $FX/t_eac3.mp4 "h264 eac3 "
    check "fixes: fast trim of H.264 + MP3 into MP4" trimmed_codecs fixtures/probe/h264_mp3.ts $FX/t_mp3ts.mp4 "h264 mp3 "
    check "fixes: fast trim of VP8 + Vorbis into MP4" trimmed_codecs fixtures/readers/vp8_vorbis.webm $FX/t_vorbis.mp4 "vp8 vorbis "
    check "fixes: fast trim of H.264 + AC-3 into Matroska" trimmed_codecs fixtures/probe/h264_ac3.ts $FX/t_ac3.mkv "h264 ac3 "
    check "fixes: strip-audio of Motion JPEG AVI into MP4" eval '$A strip-audio fixtures/formats/avi_mjpeg_pcm.avi $FX/s_mjpeg.mp4 > /dev/null 2>&1 && clean $FX/s_mjpeg.mp4'
    check "fixes: merge of H.264 and E-AC-3 picks Matroska and works" eval '$A merge fixtures/probe/h264.mp4 fixtures/probe/h264_eac3.ts $FX/m_eac3 > /dev/null 2>&1 && clean $FX/m_eac3.mkv'
    colour_of() { $FFPROBE -v error -select_streams v:0 -show_entries stream=color_range,color_space,color_transfer,color_primaries -of csv=p=0 "$1"; }
    same_colour() { $A "$1" "$2" "$3" "${@:4}" > /dev/null 2>&1 && [ "$(colour_of "$2")" = "$(colour_of "$3")" ]; }
    check "fixes: a trim of an untagged video stays untagged (MP4)" same_colour trim fixtures/probe/h264.mp4 $FX/c_trim.mp4 --start 0.2 --end 1.5
    check "fixes: a rewrap of an untagged video stays untagged (Matroska)" same_colour rewrap fixtures/probe/h264.mp4 $FX/c_wrap.mkv
    check "fixes: a rewrap of a BT.709 video keeps BT.709 (Matroska)" same_colour rewrap fixtures/probe/colr.mp4 $FX/c_tagged.mkv
    $F -i fixtures/probe/h264.mp4 -c copy -bsf:v h264_metadata=matrix_coefficients=6:colour_primaries=6:transfer_characteristics=6 $FX/vui601.mp4
    check "fixes: a trim of a video labelled BT.601 only in the stream keeps BT.601" same_colour trim $FX/vui601.mp4 $FX/c_601.mp4 --start 0.2 --end 1.5
    $A gif-video fixtures/tools/hold.gif $FX/hold.mp4 $FX/hold_matte.mp4 > /dev/null 2>&1
    check "fixes: GIF to video keeps the 3 s hold of the last frame (FFmpeg reads 3.4 s)" [ "$($FFPROBE -v error -show_entries format=duration -of csv=p=0 $FX/hold.mp4)" = 3.400000 ]
    $F -f rawvideo -pix_fmt bgra -s 64x64 -i $FX/chroma.bgra -frames:v 1 -vf scale=out_color_matrix=bt709:out_range=tv -pix_fmt yuv420p -f rawvideo $FX/chroma_ff.yuv
    chroma_like_ffmpeg() {
        python3 -c "
import sys
ours = open(sys.argv[1], 'rb').read()[4096:]
theirs = open(sys.argv[2], 'rb').read()[4096:]
gaps = [abs(a - b) for a, b in zip(ours, theirs)]
sys.exit(0 if len(ours) == len(theirs) == 2048 and max(gaps) <= 16 and sum(gaps) / len(gaps) <= 2 else 1)" "$1" $FX/chroma_ff.yuv
    }
    check "fixes: RGB to 4:2:0 keeps the colour of 1-pixel lines like swscale (chroma within 16 levels)" chroma_like_ffmpeg $FX/chroma.yuv
    for k in 1 2 3 4 5 6 7 8; do cat $FX/chroma.bgra; done > $FX/chroma8.bgra
    encoded_chroma() { $A frames-in $FX/chroma.mp4 --size 64x64 --fps 30 --format bgra --bitrate 20000000 --in $FX/chroma8.bgra > /dev/null 2>&1 && $F -i $FX/chroma.mp4 -frames:v 1 -pix_fmt yuv420p -f rawvideo $FX/chroma_enc.yuv && chroma_like_ffmpeg $FX/chroma_enc.yuv; }
    check "fixes: BGRA frames encoded at 20 Mbit/s keep the colour of 1-pixel lines (chroma within 16 levels of swscale's)" encoded_chroma
    $F -f lavfi -i sine=r=48000:d=3 -ac 1 -f f32le $FX/mono.f32
    mono_rate() { $A aencode $FX/mono_$1.m4a --pcm $FX/mono.f32 --rate 48000 --channels 1 --format aac --bitrate $1 > /dev/null 2>&1 && [ "$($FFPROBE -v error -show_entries stream=bit_rate -of csv=p=0 $FX/mono_$1.m4a)" -ge 120000 ]; }
    for rate in 192000 256000 320000; do check "fixes: mono AAC at $((rate / 1000)) kbit/s is not cut to 64 kbit/s" mono_rate $rate; done
    $F -f lavfi -i testsrc2=s=320x180:d=4 -f lavfi -i sine=d=4 -c:v libx264 -c:a aac -metadata title=Holiday -metadata location=+52.3700+004.8900/ -metadata creation_time=2024-05-06T07:08:09Z -f mov $FX/phone.mov
    kept_tags() { $A trim $FX/phone.mov "$1" --start 1 --end 3 > /dev/null 2>&1 && local tags=$($FFPROBE -v error -show_entries format_tags -of compact=p=0 "$1") && [[ "$tags" == *"title=Holiday"* ]] && echo "$tags" | grep -qi "location=+52.3700+004.8900/"; }
    for ext in mp4 mov mkv; do check "fixes: a trim into .$ext keeps the title and location" kept_tags $FX/phone_trim.$ext; done
    created() { $A "$1" $FX/phone.mov "${@:2}" > /dev/null 2>&1 && $FFPROBE -v error -show_entries format_tags=creation_time -of csv=p=0 "$2" | grep -q "^2024-05-06T07:08:09"; }
    check "fixes: compress keeps the creation time" created compress $FX/phone_small.mp4 --size 160x90 --audio none
    check "fixes: reverse keeps the creation time" created reverse $FX/phone_back.mp4
    rotation_of() { $FFPROBE -v error -select_streams v:0 -show_entries stream_side_data=rotation -of csv=p=0 "$1" | tr -d ',\r\n'; }
    turned() { $A $1 fixtures/probe/rot90.mp4 $2 > /dev/null 2>&1 && [ -n "$(rotation_of $2)" ] && [ "$(rotation_of $2)" = "$(rotation_of fixtures/probe/rot90.mp4)" ]; }
    check "fixes: rewrap of a rotated MP4 into Matroska keeps the rotation" turned rewrap $FX/rot.mkv
    check "fixes: strip-audio of a rotated MP4 into Matroska keeps the rotation" turned strip-audio $FX/rot_silent.mkv
    check "fixes: the hand-made MPEG-1 Layer I file sounds as FFmpeg decodes it" python3 audio.py against $FX/tone.mp1 40 0 0
    $F -f lavfi -i testsrc2=s=320x180:r=30:d=6 -f lavfi -i sine=d=6 -c:v libx264 -preset veryfast -bf 2 -x264-params open-gop=1:keyint=30:min-keyint=30:scenecut=0 -c:a aac -f mpegts $FX/open_gop.ts
    $F -ss 2.5 -i $FX/open_gop.ts -c copy -f mpegts $FX/open_gop_cut.ts
    python3 -c "
import sys
data = open(sys.argv[1], 'rb').read()
packets = [data[i:i + 188] for i in range(0, len(data) - 187, 188)]
video = [k for k, p in enumerate(packets) if ((p[1] & 0x1F) << 8 | p[2]) == 256 and not p[1] & 0x40]
del packets[video[len(video) // 2]]
open(sys.argv[2], 'wb').write(b''.join(packets))" $FX/open_gop.ts $FX/open_gop_lost.ts
    ts_pictures() { $A packets "$1" | awk '$1 == "track" && $3 == "video" { for (i = 1; i < NF; ++i) if ($i == "packets") print $(i + 1) }'; }
    check "fixes: a transport stream cut at an I picture that is not an IDR keeps its video" [ "$(ts_pictures $FX/open_gop_cut.ts)" -ge 80 ]
    check "fixes: H.264 in a transport stream with one IDR goes on after a lost packet" [ "$(ts_pictures $FX/open_gop_lost.ts)" -ge 140 ]
    $F -f lavfi -i testsrc2=s=320x180:r=30:d=2 -f lavfi -i sine=d=2 -c:v libx264 -preset veryfast -c:a aac -output_ts_offset 20 -f mpegts $FX/splice_a.ts
    $F -f lavfi -i testsrc2=s=320x180:r=30:d=2 -f lavfi -i sine=d=2 -c:v libx264 -preset veryfast -c:a aac -f mpegts $FX/splice_b.ts
    cat $FX/splice_a.ts $FX/splice_b.ts > $FX/spliced.ts
    in_step() {
        $A packets "$1" | python3 -c "
import sys
tracks, kind = {}, None
for line in sys.stdin:
    word = line.split()
    if word[0] == 'track': kind = word[2]; tracks[kind] = []
    elif kind and len(word) == 6 and word[0].lstrip('-').isdigit(): tracks[kind].append((int(word[0]), int(word[1])))
video, audio = tracks.get('video', []), tracks.get('audio', [])
steady = all(b[1] >= a[1] for t in (video, audio) for a, b in zip(t, t[1:]))
sys.exit(0 if steady and len(video) >= 118 and max(p for p, d in video) - min(p for p, d in video) < 4.5 * 90000 else 1)"
    }
    check "fixes: two transport streams joined with their clocks restarting play one after the other" in_step $FX/spliced.ts
    $F -f lavfi -i testsrc2=s=320x180:r=30:d=4 -f lavfi -i sine=d=4 -c:v libx264 -preset veryfast -c:a aac -output_ts_offset 5000 -mpegts_service_id 2 -mpegts_pmt_start_pid 0x1100 -mpegts_start_pid 0x200 -f mpegts $FX/program_b.ts
    python3 -c "
import sys
a, b = (open(name, 'rb').read() for name in sys.argv[1:3])
a, b = ([data[i:i + 188] for i in range(0, len(data) - 187, 188)] for data in (a, b))
mixed = sorted([(i / len(a), 0, p) for i, p in enumerate(a)] + [(i / len(b), 1, p) for i, p in enumerate(b)])
open(sys.argv[3], 'wb').write(b''.join(p for _, _, p in mixed))" $FX/spliced.ts $FX/program_b.ts $FX/two_programs.ts
    programs_steady() {
        $A packets "$1" | python3 -c "
import sys
tracks = []
for line in sys.stdin:
    word = line.split()
    if word and word[0] == 'track': tracks.append([])
    elif tracks and len(word) == 6 and word[0].lstrip('-').isdigit(): tracks[-1].append((int(word[0]), int(word[1])))
steady = all(b[1] >= a[1] for t in tracks for a, b in zip(t, t[1:]))
sys.exit(0 if len(tracks) == 4 and all(tracks) and steady and all(max(p for p, d in t) - min(p for p, d in t) < 4.5 * 90000 for t in tracks) else 1)"
    }
    check "fixes: a spliced program next to another program with its own clock: each keeps its own steady time line" programs_steady $FX/two_programs.ts
else
    skip "fixes: FFmpeg as the judge of the written files and the 7.1 order"
fi
small_disk() {
    local source=$O/h264_1080_60s.mp4 dir=$FX/small ok=0 size
    size=$(stat -f %z $source)
    mkdir -p $dir && mount -t tmpfs -o size=$((size * 14 / 10)) tmpfs $dir || return 1
    $A rewrap $source $dir/out.mp4 > /dev/null 2>&1 && [ "$(stat -f %z $dir/out.mp4)" -gt $((size * 9 / 10)) ] && rm -f $dir/out.mp4 && $A rewrap $source $dir/out.mkv > /dev/null 2>&1 && [ ! -e $dir/out.mkv.part ] && ok=1
    umount $dir
    [ $ok = 1 ]
}
if [ "$SYSTEM" = Linux ] && [ "$(id -u)" = 0 ] && [ -f $O/h264_1080_60s.mp4 ]; then
    check "fixes: rewrap into MP4 and Matroska on a drive with 1.4 times the file free (the writers move data in place)" small_disk
else
    skip "fixes: rewrap on a small drive (needs Linux as root to mount a tmpfs)"
fi
rm -rf $FX
