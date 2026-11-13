[ -n "${QUICK:-}" ] && return 0
have_ffmpeg || return 0
echo "== speed"
python3 bench.py decode $O/h264_1080_60s.mp4 1800
python3 bench.py decode $O/hevc_1080_60s.mp4 1800
python3 bench.py encode $O/raw_1080.yuv $O/pcm.raw $O 300
python3 bench.py transcode $O/h264_1080_60s.mp4 $O 1800

