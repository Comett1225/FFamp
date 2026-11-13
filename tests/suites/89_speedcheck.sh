echo "== speed check: reading ahead stays exact, and no big slowdown (generous limits, not a benchmark)"
SP="python3 speedcheck.py"
for f in h264_270p.mp4 hevc_270p.mp4 hevc10_270p.mp4 h264_mbaff_272p.mp4 vp9_270p.mp4 av1_270p.mp4 av1_10bit_270p.mp4; do
    check "runs of frames after jumps in $f equal the straight decode" $SP steps fixtures/video/$f
done
for f in vp9_270p.mp4 vp9_10bit_270p.mp4 av1_270p.mp4 av1_10bit_270p.mp4 vp8_270p.ivf; do
    check "runs of frames after jumps in $f equal the straight decode in software" $SP steps fixtures/video/$f --software
done
check "a 1080p H.264 test clip is made without FFmpeg" $SP make $O/sp_h264.mp4 h264 150
check "a 1080p AV1 test clip is made without FFmpeg" $SP make $O/sp_av1.mp4 av1 150
check "1080p to 720p RGBA frames: over 350 per second" $SP rate 150 350 frames-out $O/sp_h264.mp4 --size 1280x720 --format rgba --out /dev/null
check "1080p RGBA frames: over 450 per second" $SP rate 150 450 frames-out $O/sp_h264.mp4 --format rgba --out /dev/null
check "1080p AV1 software decode into memory: over 400 per second" $SP rate 150 400 bench $O/sp_av1.mp4 --mode copy --software
if have_ffmpeg; then
    $F -i $O/sp_h264.mp4 -c:v libx265 -preset ultrafast -x265-params log-level=error -pix_fmt yuv420p10le -tag:v hvc1 $O/sp_hevc10.mp4
    check "1080p 10-bit HEVC decode into memory: over 800 per second" $SP rate 150 800 bench $O/sp_hevc10.mp4 --mode copy
    $A frames-out $O/sp_h264.mp4 --size 1280x720 --format rgba --count 3 --out $O/sp_a.raw
    $F -i $O/sp_h264.mp4 -frames:v 3 -vf scale=1280:720:flags=accurate_rnd+full_chroma_int+bicubic -pix_fmt rgba -f rawvideo $O/sp_b.raw
    check "1080p to 720p RGBA is close to FFmpeg's bicubic scaling" python3 capturecheck.py psnr $O/sp_a.raw $O/sp_b.raw 45
else
    skip "1080p 10-bit HEVC decode into memory (needs FFmpeg to make the clip)"
    skip "1080p to 720p RGBA against FFmpeg's bicubic scaling"
fi
rm -f $O/sp_h264.mp4 $O/sp_av1.mp4 $O/sp_hevc10.mp4 $O/sp_vp9.webm $O/sp_a.raw $O/sp_b.raw
