echo "== formats: AC-3, AMR, CAF, AVI and older video fixtures against checked tables (no FFmpeg)"
FX=fixtures/formats
FC="python3 formats_check.py"
FA_AUDIO="ac3_51.ac3 eac3_51.eac3 ac3_stereo.mp4 eac3_51.mp4 ac3_51.mov ac3_mono.mka ac3_21.ac3 amr_nb.amr amr_wb.amr amr_nb.3gp amr_wb.3gp caf_s16be.caf caf_s24le.caf caf_f32le.caf caf_aac.caf caf_he_aac.caf caf_alac.caf caf_flac.caf caf_opus.caf caf_ac3.caf caf_mp3.caf caf_amr.caf avi_mpeg4_mp3.avi avi_asp_ac3.avi avi_mjpeg_pcm.avi avi_mjpeg422_pcm.avi avi_mpeg2_mp2.avi avi_mpeg1_pcm.avi avi_dv_pcm.avi"
FA_VIDEO="mpeg4.mp4 mpeg4.mkv mjpeg.mov mjpeg422.mov mjpeg444.mkv mpeg2.mp4 mpeg2.mov mpeg2.mkv mpeg1.mov mpeg1.mkv dv_pal.mov dv_ntsc.mov h263.3gp avi_mpeg4_mp3.avi avi_mjpeg_pcm.avi avi_mjpeg422_pcm.avi avi_h264_mp3.avi avi_mpeg2_mp2.avi avi_mpeg1_pcm.avi avi_dv_pcm.avi"
listed() { [ "$("${@:3}" 2> /dev/null | md5)" = "$(grep " $2\$" $FX/$1.md5 | cut -d' ' -f1)" ]; }
FX_XVID=
$A videoinfo $FX/mpeg4.mp4 2> /dev/null | grep -q "backend xvid" && FX_XVID=xvid_
for f in $(cut -d' ' -f3 $FX/packets.md5); do
    check "packets of $f match the checked table" listed packets $f $A packets $FX/$f
    check "probe of $f matches the checked output" listed probe $f $A probe --json $FX/$f
done
for f in $FA_AUDIO; do case $f in *aac*|*opus*|*mp3*|*mp2*|*ac3*) judge=float_check ;; *) judge=check ;; esac; $judge "$f sound decodes to the checked checksum" listed sound $f $A adecode $FX/$f; done
for f in $FA_VIDEO; do
    table=pictures
    case $f in mpeg4*|avi_mpeg4*) table=${FX_XVID}pictures ;; esac
    check "$f pictures decode to the checked checksum" listed $table $f $A decode $FX/$f
done

echo "== formats: known signals, random access, the same stream in different wrappings (no FFmpeg)"
while read f rate channels seconds least; do
    check "$f: rate, channels, length to the sample, no offset from its source signal" python3 audio.py truth $FX/$f $rate $channels $seconds $least 0
done <<TABLE
ac3_mono.mka 44100 1 1.5 25
caf_s16be.caf 44100 2 0.5 200
caf_s24le.caf 44100 2 0.5 200
caf_f32le.caf 44100 2 0.5 200
caf_aac.caf 44100 2 1.5 30
caf_he_aac.caf 44100 2 1.5 20
caf_alac.caf 44100 2 1.5 200
caf_flac.caf 44100 2 1.5 200
caf_opus.caf 48000 2 1.5 18
TABLE
for f in ac3_51.ac3 eac3_51.eac3 ac3_stereo.mp4 eac3_51.mp4 ac3_21.ac3 amr_nb.amr amr_wb.3gp caf_s24le.caf caf_aac.caf caf_he_aac.caf caf_alac.caf caf_flac.caf caf_opus.caf caf_ac3.caf caf_mp3.caf avi_mpeg4_mp3.avi avi_asp_ac3.avi avi_mjpeg422_pcm.avi avi_mpeg2_mp2.avi; do
    check "random reads $f" python3 audio.py chunks adecode $FX/$f 30 3
done
check "AC-3 5.1 to stereo levels" python3 audio.py mixing $FX/ac3_51.ac3 2
check "AC-3 5.1 as 44.1 kHz stereo: random reads" python3 audio.py chunks pcm $FX/ac3_51.ac3 30 4 --rate 44100 --channels 2
for f in $FA_VIDEO; do check "random frames of $f equal the straight decode" python3 comparex.py selfseek $FX/$f 16 5; done
fx_columns() { $A packets $FX/$1 --track ${3:-0} | grep -E '^-?[0-9]' | cut -d' ' -f"$2" | md5; }
fx_same() { [ "$(fx_columns $1 $3 ${4:-0})" = "$(fx_columns $2 $3 ${5:-0})" ]; }
check "AC-3 frames are the same from a raw file and from CAF" fx_same ac3_51.ac3 caf_ac3.caf 1-6
check "AC-3 frames are the same from a raw file and from MOV" fx_same ac3_51.ac3 ac3_51.mov 3-6
check "E-AC-3 frames are the same from a raw file and from MP4" fx_same eac3_51.eac3 eac3_51.mp4 3-6
check "AMR-NB frames are the same from a raw file and from 3GP" fx_same amr_nb.amr amr_nb.3gp 1-6
check "AMR-NB frames are the same from a raw file and from CAF" fx_same amr_nb.amr caf_amr.caf 1-6
check "AMR-WB frames are the same from a raw file and from 3GP" fx_same amr_wb.amr amr_wb.3gp 1-6
check "MPEG-2 pictures are the same from MP4 and from Matroska" fx_same mpeg2.mp4 mpeg2.mkv 4-6
check "MPEG-1 pictures are the same from MOV and from Matroska" fx_same mpeg1.mov mpeg1.mkv 4-6
check "MPEG-4 in Matroska decodes to the MP4 file's frames" [ "$($A decode $FX/mpeg4.mkv | md5)" = "$($A decode $FX/mpeg4.mp4 | md5)" ]
check "MPEG-2 in MOV and Matroska decodes to the MP4 file's frames" [ "$($A decode $FX/mpeg2.mov | md5)$($A decode $FX/mpeg2.mkv | md5)" = "$($A decode $FX/mpeg2.mp4 | md5)$($A decode $FX/mpeg2.mp4 | md5)" ]
check "a DivX packed bitstream is unpacked to the times of the same stream in MP4" $FC times $FX/avi_packed.avi $FX/mpeg4_asp.mp4
check "AVI with B-frames has the times of the same stream in MP4" $FC times $FX/avi_asp_ac3.avi $FX/mpeg4_asp.mp4
check "the packed bitstream has one packet for each picture" [ "$($A packets $FX/avi_packed.avi | grep -cE '^-?[0-9]')" = 20 ]
refuses_with() { ! $A decode $1 > /dev/null 2> $O/fx_refused.txt && grep -q "$2" $O/fx_refused.txt; }
if [ -n "$FX_XVID" ]; then
    fx_asp() { [ "$($A decode $FX/$1 | md5)" = "$($A decode $FX/mpeg4_asp.mp4 | md5)" ] && [ "$($A decode $FX/$1 | wc -c)" -eq 576000 ]; }
    check "MPEG-4 Advanced Simple Profile plays through Xvid: the AVI gives the MP4 file's pictures" fx_asp avi_asp_ac3.avi
    check "and the packed AVI too" fx_asp avi_packed.avi
else
    check "MPEG-4 Advanced Simple Profile is refused with its own message" refuses_with $FX/mpeg4_asp.mp4 "Advanced Simple Profile"
    check "the packed AVI is refused with the same message" refuses_with $FX/avi_packed.avi "Advanced Simple Profile"
fi
printf '\x30\x26\xb2\x75\x8e\x66\xcf\x11\xa6\xd9\x00\xaa\x00\x62\xce\x6c' > $O/fx_wma.bin
head -c 3000 /dev/zero >> $O/fx_wma.bin
check "a Windows Media file is refused with a message" refuses_with $O/fx_wma.bin "Windows Media"
for f in avi_mpeg4_mp3.avi avi_asp_ac3.avi avi_mjpeg_pcm.avi avi_h264_mp3.avi avi_mpeg2_mp2.avi avi_dv_pcm.avi; do
    python3 $FX/craft.py renamed $FX/$f $O/fx_old.avi indx
    python3 $FX/craft.py renamed $FX/$f $O/fx_walk.avi indx idx1
    check "$f read through idx1 alone gives the same packets" [ "$($A packets $O/fx_old.avi | md5)" = "$($A packets $FX/$f | md5)" ]
    check "$f read without any index gives the same packets" [ "$($A packets $O/fx_walk.avi | md5)" = "$($A packets $FX/$f | md5)" ]
done

echo "== formats: alpha, one undecodable sample, unknown codecs (no FFmpeg)"
for f in alpha_prores.mov alpha_vp9.webm alpha_vp8.webm; do
    check "$f decodes with its alpha plane, at full and at half size" $FC veil $FX/$f 160 120
    check "$f as BGRA decodes to the checked checksum" listed veiled $f $A decode $FX/$f --bgra
    check "random frames of $f equal the straight decode" python3 comparex.py selfseek $FX/$f 10 5
done
check "probe names the alpha pixel formats" [ "$($A probe --json $FX/alpha_prores.mov | grep -c yuva444p12le)$($A probe --json $FX/alpha_vp9.webm | grep -c yuva420p)$($A probe --json $FX/alpha_vp8.webm | grep -c yuva420p)" = 111 ]
check "a file with one undecodable sample still gives every frame, the lost ones as the frame before" $FC concealed $FX/bad_sample.mp4 160 120 115 117 119
check "bad_sample.mp4 decodes to the checked checksum" listed pictures bad_sample.mp4 $A decode $FX/bad_sample.mp4
names_unknown() { $A probe --json $FX/unknown_codec.mkv | grep -q '"codec_name":"ffv1".*"start_time":"0.000000".*"duration":"0.400000"'; }
check "a video codec nothing here decodes is still named and timed by probe" names_unknown
check "and its decoder says it is not supported" refuses_with $FX/unknown_codec.mkv "video codec not supported"

echo "== formats: damaged files never crash (no FFmpeg)"
FX_FUZZ=30
[ -n "${QUICK:-}" ] && FX_FUZZ=6
check "cut, flipped, shortened and padded fixtures never crash" python3 fuzz.py $O/fx_fuzz.bin $FX_FUZZ $FX/*.ac3 $FX/*.eac3 $FX/*.amr $FX/*.3gp $FX/*.caf $FX/*.avi $FX/*.mov $FX/*.mkv $FX/*.mka $FX/*.mp4
for f in avi_mpeg4_mp3.avi avi_h264_mp3.avi avi_mpeg2_mp2.avi mjpeg422.mov mpeg2.mkv dv_pal.mov; do check "damaged copies of $f decode or fail cleanly" python3 comparex.py fuzz $FX/$f $FX_FUZZ 9; done
rm -f $O/fx_*

have_ffmpeg || { skip "formats against ffmpeg and ffprobe"; return 0; }

echo "== formats: sound against ffmpeg's own decode"
while read f least; do
    check "$f against ffmpeg" python3 audio.py against $FX/$f $least 0 0
done <<TABLE
ac3_51.ac3 50
eac3_51.eac3 50
ac3_stereo.mp4 50
eac3_51.mp4 50
ac3_51.mov 50
ac3_mono.mka 60
caf_ac3.caf 50
amr_nb.amr 30
amr_wb.amr 30
amr_nb.3gp 30
amr_wb.3gp 30
caf_amr.caf 30
caf_aac.caf 100
caf_opus.caf 100
caf_mp3.caf 75
avi_mpeg4_mp3.avi 75
avi_mpeg2_mp2.avi 70
avi_asp_ac3.avi 25
TABLE
for f in caf_s16be.caf caf_s24le.caf caf_f32le.caf caf_alac.caf caf_flac.caf avi_mjpeg_pcm.avi avi_mjpeg422_pcm.avi avi_mpeg1_pcm.avi avi_dv_pcm.avi; do
    check "$f bit-exact against ffmpeg" python3 audio.py against $FX/$f 200 0 0 exact
done
check "AC-3 2.1 comes out as 5.1 with every channel in its place" $FC layout $FX/ac3_21.ac3 "5.1|FL=FL|FR=FR|LFE=LFE" 6 45

echo "== formats: pictures against ffmpeg's own decode"
while read f floor; do
    check "decode $f" python3 comparex.py decode $FX/$f $floor
done <<TABLE
mpeg4.mp4 45
mpeg4.mkv 45
mjpeg.mov 60
mpeg2.mp4 48
mpeg2.mov 48
mpeg2.mkv 48
mpeg1.mov 48
mpeg1.mkv 48
dv_pal.mov 33
dv_ntsc.mov 33
h263.3gp 55
avi_mpeg4_mp3.avi 45
avi_mjpeg_pcm.avi 60
avi_mpeg2_mp2.avi 48
avi_mpeg1_pcm.avi 48
avi_dv_pcm.avi 33
TABLE
check "decode avi_h264_mp3.avi (B-frames put in order) frame-exact" python3 comparex.py decode $FX/avi_h264_mp3.avi
for f in mjpeg422.mov mjpeg444.mkv avi_mjpeg422_pcm.avi; do check "decode $f through ImageIO (as BGRA; the judge fills in 4:2:2 colour with another filter)" $FC bgra $FX/$f 30; done

check "alpha_prores.mov alpha against ffmpeg" $FC veil $FX/alpha_prores.mov 160 120 -an
check "alpha_vp9.webm alpha against ffmpeg with libvpx" $FC veil $FX/alpha_vp9.webm 160 120 -c:v libvpx-vp9
check "alpha_vp8.webm alpha against ffmpeg with libvpx" $FC veil $FX/alpha_vp8.webm 160 120 -c:v libvpx
check "decode alpha_prores.mov colour" python3 comparex.py decode $FX/alpha_prores.mov 50
check "decode alpha_vp9.webm colour" python3 comparex.py decode $FX/alpha_vp9.webm
check "decode alpha_vp8.webm colour" python3 comparex.py decode $FX/alpha_vp8.webm
check "fields of unknown_codec.mkv" python3 probe_check.py compare $FX/unknown_codec.mkv stream0.pix_fmt stream0.color_range stream0.bit_rate
check "fields of alpha_prores.mov" python3 probe_check.py compare $FX/alpha_prores.mov

echo "== formats: probe fields against ffprobe"
for f in ac3_stereo.mp4 eac3_51.mp4 ac3_51.mov ac3_mono.mka amr_nb.3gp amr_wb.3gp mpeg4.mp4 mpeg4_asp.mp4 mjpeg.mov mjpeg422.mov dv_pal.mov dv_ntsc.mov h263.3gp; do
    check "fields of $f" python3 probe_check.py compare $FX/$f
done
for f in mpeg2.mp4 mpeg2.mov mpeg1.mov; do check "fields of $f (no profile name; ffprobe's MPEG-1 bit rate is a placeholder)" python3 probe_check.py compare $FX/$f stream0.profile stream0.bit_rate; done
for f in mpeg4.mkv mjpeg444.mkv mpeg2.mkv mpeg1.mkv; do check "fields of $f" python3 probe_check.py compare $FX/$f stream0.profile stream0.bit_rate stream0.color_range; done
for f in ac3_51.ac3 eac3_51.eac3; do check "fields of $f (our time base is the sample rate)" python3 probe_check.py compare $FX/$f stream0.time_base; done
for f in amr_nb.amr amr_wb.amr; do check "fields of $f (ffprobe guesses the length from the bit rate)" python3 probe_check.py compare $FX/$f duration bit_rate start_time; done
for f in caf_s16be.caf caf_s24le.caf caf_f32le.caf caf_aac.caf caf_alac.caf caf_flac.caf caf_opus.caf caf_ac3.caf caf_mp3.caf caf_amr.caf; do
    check "fields of $f (ffprobe counts sample frames, we count packets)" python3 probe_check.py compare $FX/$f stream0.nb_frames stream0.bit_rate
done
for f in avi_mpeg4_mp3.avi avi_asp_ac3.avi avi_mjpeg_pcm.avi avi_mjpeg422_pcm.avi avi_h264_mp3.avi avi_mpeg2_mp2.avi avi_mpeg1_pcm.avi avi_dv_pcm.avi; do
    check "fields of $f (sound timed by its own frames, not by AVI blocks)" python3 probe_check.py compare $FX/$f bit_rate nb_frames avg_frame_rate stream1.time_base stream1.duration stream1.sample_fmt format.duration stream0.profile stream0.color_range
done

if [ -z "${QUICK:-}" ]; then
    echo "== formats: an AVI file over 1 GB (OpenDML, more than one RIFF part)"
    $F -f lavfi -i "testsrc2=size=1920x1080:rate=25,noise=alls=40:allf=t" -f lavfi -i sine=frequency=440:sample_rate=48000 -t 30 -c:v mjpeg -q:v 1 -pix_fmt yuvj420p -c:a pcm_s16le $O/fx_big.avi
    check "the file is over 1 GB" [ "$(stat -f %z $O/fx_big.avi)" -gt 1073741824 ]
    check "the file has a second RIFF part" [ "$(LC_ALL=C grep -a -c AVIX $O/fx_big.avi)" -ge 1 ]
    check "frames from the start to the end of the big AVI against ffmpeg" $FC late $O/fx_big.avi 6 3 45
    check "sound of the big AVI: length equals ffmpeg's decode" [ "$($A adecode $O/fx_big.avi --frames 0 2>&1)" = "48000 Hz, 1 channels, $(($($FFMPEG -nostdin -v error -i $O/fx_big.avi -vn -f s16le - | wc -c) / 2)) frames" ]
    rm -f $O/fx_big.avi
fi
