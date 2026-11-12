import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.environ.get("AMEDIA", os.path.join(HERE, "..", "build", "amedia"))
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
QUIET = [FFMPEG, "-nostdin", "-hide_banner", "-v", "error", "-y"]
SAME = "setparams=range=tv:colorspace=bt709:color_primaries=bt709:color_trc=bt709"


def timed(label, command, frames, runs=3, piped=False):
    best = None
    for _ in range(runs):
        start = time.perf_counter()
        if piped:
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            while process.stdout.read(1 << 22):
                pass
            if process.wait() != 0:
                raise RuntimeError(label)
        else:
            subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        spent = time.perf_counter() - start
        best = spent if best is None else min(best, spent)
    print(f"  {label:<58} {best:7.3f} s  {frames / best:8.1f} fps")
    return best


def decode(path, frames):
    print(f"decode {os.path.basename(path)} ({frames} frames, best of 3, whole process time)")
    timed("ours: frames stay on the GPU (async)", [AMEDIA, "bench", path, "--mode", "async"], frames)
    timed("ours: frame by frame in display order, on the GPU", [AMEDIA, "bench", path, "--mode", "frames"], frames)
    timed("ours: frame by frame, copied to memory as I420", [AMEDIA, "bench", path, "--mode", "copy"], frames)
    timed("ffmpeg: VideoToolbox, frames stay on the GPU", QUIET + ["-hwaccel", "videotoolbox", "-hwaccel_output_format", "videotoolbox_vld", "-i", path, "-map", "0:v:0", "-f", "null", "-"], frames)
    timed("ffmpeg: VideoToolbox, copied to memory", QUIET + ["-hwaccel", "videotoolbox", "-i", path, "-map", "0:v:0", "-f", "null", "-"], frames)
    timed("ffmpeg: software decode", QUIET + ["-i", path, "-map", "0:v:0", "-f", "null", "-"], frames)
    timed("ffmpeg: software, raw I420 through a pipe", QUIET + ["-i", path, "-map", "0:v:0", "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"], frames, piped=True)
    timed("ffmpeg: software, raw RGBA through a pipe (app today)", QUIET + ["-i", path, "-an", "-vf", "scale=1920:1080", "-pix_fmt", "rgba", "-f", "rawvideo", "-"], frames, piped=True)


def encode(raw, pcm, out, frames):
    print(f"encode {frames} frames 1920x1080 from raw I420 + stereo PCM (best of 3, whole process time)")
    for codec, rate in (("h264", "10000000"), ("hevc", "6000000")):
        ours = os.path.join(out, f"bench_ours_{codec}.mp4")
        theirs = os.path.join(out, f"bench_ffmpeg_{codec}.mp4")
        timed(f"ours: {codec} VideoToolbox + AAC, own MP4 writer",
              [AMEDIA, "encode", ours, "--video", raw, "--size", "1920x1080", "--fps", "30", "--frames", str(frames), "--codec", codec, "--bitrate", rate, "--audio", pcm], frames)
        timed(f"ffmpeg: {codec}_videotoolbox + aac_at, faststart",
              QUIET + ["-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "30", "-i", raw, "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", pcm,
                       "-frames:v", str(frames), "-c:v", f"{codec}_videotoolbox", "-b:v", rate, "-g", "30", "-realtime", "0"] + (["-profile:v", "high"] if codec == "h264" else ["-tag:v", "hvc1"]) +
              ["-c:a", "aac_at", "-b:a", "160k", "-shortest", "-movflags", "+faststart", theirs], frames)
        for name, path in (("ours", ours), ("ffmpeg", theirs)):
            result = subprocess.run([FFMPEG, "-nostdin", "-hide_banner", "-i", path, "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "30", "-i", raw,
                                     "-frames:v", str(frames), "-lavfi", f"[0:v]{SAME}[a];[1:v]{SAME}[b];[a][b]psnr", "-f", "null", "-"], capture_output=True, text=True)
            line = [l for l in result.stderr.splitlines() if "PSNR" in l]
            size = os.path.getsize(path)
            print(f"    {name:<7} {codec} file {size / 1e6:6.2f} MB  {line[-1].split('] ')[-1] if line else 'no PSNR'}")


def export(raw, pcm, out, frames):
    print(f"export path: {frames} frames 1920x1080 from raw BGRA + stereo PCM (best of 3, whole process time)")
    for codec, rate in (("h264", "10000000"), ("hevc", "6000000")):
        ours = os.path.join(out, f"bench_ours_bgra_{codec}.mp4")
        theirs = os.path.join(out, f"bench_ffmpeg_bgra_{codec}.mp4")
        timed(f"ours: BGRA straight into {codec} VideoToolbox",
              [AMEDIA, "encode", ours, "--video", raw, "--format", "bgra", "--size", "1920x1080", "--fps", "30", "--frames", str(frames), "--codec", codec, "--bitrate", rate, "--audio", pcm], frames)
        timed(f"ffmpeg: BGRA, scale filter, {codec}_videotoolbox (app today)",
              QUIET + ["-f", "rawvideo", "-pix_fmt", "bgra", "-s", "1920x1080", "-r", "30", "-i", raw, "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", pcm, "-frames:v", str(frames),
                       "-vf", "scale=out_color_matrix=bt709:out_range=tv", "-c:v", f"{codec}_videotoolbox", "-realtime", "0"] + (["-profile:v", "high"] if codec == "h264" else ["-tag:v", "hvc1"]) +
              ["-b:v", rate, "-g", "30", "-pix_fmt", "yuv420p", "-c:a", "aac_at", "-b:a", "160k", "-shortest", "-movflags", "+faststart", theirs], frames)


def transcode(path, out, frames):
    print(f"compress {os.path.basename(path)} to HEVC 4 Mbit/s, audio copied ({frames} frames, best of 3)")
    timed("ours: decode and encode in one process, no copies", [AMEDIA, "transcode", path, os.path.join(out, "bench_ours_compress.mp4"), "--codec", "hevc", "--bitrate", "4000000"], frames)
    timed("ffmpeg: hwaccel videotoolbox to hevc_videotoolbox",
          QUIET + ["-hwaccel", "videotoolbox", "-hwaccel_output_format", "videotoolbox_vld", "-i", path, "-map", "0:v:0", "-map", "0:a:0?", "-c:v", "hevc_videotoolbox", "-b:v", "4000000",
                   "-g", "30", "-tag:v", "hvc1", "-c:a", "copy", "-movflags", "+faststart", os.path.join(out, "bench_ffmpeg_compress.mp4")], frames)
    timed("ffmpeg: software decode to hevc_videotoolbox (app today)",
          QUIET + ["-i", path, "-map", "0:v:0", "-map", "0:a:0?", "-c:v", "hevc_videotoolbox", "-b:v", "4000000", "-g", "30", "-tag:v", "hvc1", "-c:a", "copy", "-movflags", "+faststart",
                   os.path.join(out, "bench_ffmpeg_compress.mp4")], frames)


if __name__ == "__main__":
    kind = sys.argv[1]
    if kind == "decode":
        decode(sys.argv[2], int(sys.argv[3]))
    elif kind == "encode":
        encode(sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]))
    elif kind == "export":
        export(sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]))
    elif kind == "transcode":
        transcode(sys.argv[2], sys.argv[3], int(sys.argv[4]))
