import os
import platform
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.environ.get("AMEDIA", os.path.join(HERE, "..", "build", "ffamp"))
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
FFPROBE = os.environ.get("FFPROBE", "ffprobe")
WINDOWS = platform.system() == "Windows"


def best(commands, runs=3):
    fastest = None
    for _ in range(runs):
        start = time.perf_counter()
        for command in commands:
            if subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0:
                return None
        spent = time.perf_counter() - start
        fastest = spent if fastest is None else min(fastest, spent)
    return fastest


def works(command):
    return subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def row(task, ours, theirs, note=""):
    shown = lambda t: "fails" if t is None else (f"{t * 1000:.0f} ms" if t < 10 else f"{t:.1f} s")
    ratio = f"{theirs / ours:.2f}x" if ours and theirs else ""
    print(f"| {task} | {shown(ours)} | {shown(theirs)} | {ratio} | {note} |", flush=True)


def length_of(path):
    out = subprocess.run([FFPROBE, "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path], capture_output=True, text=True).stdout
    return float(out.strip().split()[0])


def h264_encoder():
    if WINDOWS:
        for name in ("h264_amf", "h264_nvenc", "h264_qsv", "h264_mf"):
            if works([FFMPEG, "-v", "error", "-f", "lavfi", "-i", "color=c=black:s=1920x1080:r=30:d=0.2", "-c:v", name, "-b:v", "8M", "-frames:v", "2", "-f", "null", "-"]):
                return name
    return "libx264"


def aac_encoder(kbps):
    return "aac_mf" if WINDOWS and kbps <= 192 else "aac"


def main(folder):
    clips = [os.path.join(folder, name) for name in ("h264_1080_60s.mp4", "hevc_1080_60s.mp4") if os.path.exists(os.path.join(folder, name))]
    raw, pcm = os.path.join(folder, "raw_1080.yuv"), os.path.join(folder, "pcm.raw")
    scratch = os.path.join(folder, "speed")
    os.makedirs(scratch, exist_ok=True)
    print(f"| Task | FFamp | FFmpeg | FFamp faster by | Note |")
    print("|---|---|---|---|---|")
    for clip in clips:
        name = os.path.basename(clip)
        seconds = length_of(clip)
        row(f"open + probe, {name}", best([[AMEDIA, "probe", "--json", clip]], 5), best([[FFPROBE, "-v", "error", "-show_streams", "-show_format", "-of", "json", clip]], 5))
        row(f"frame index, {name}", best([[AMEDIA, "packets", clip]], 5),
            best([[FFPROBE, "-v", "error", "-select_streams", "0", "-show_entries", "packet=pts_time,dts_time,size,pos,flags", "-of", "compact=p=0", clip]], 5))
        times = [seconds * (k + 0.5) / 12 for k in range(12)]
        ours = [[AMEDIA, "frames-out", clip, "--size", "160x90", "--start", f"{t:.3f}", "--count", "1", "--out", os.path.join(scratch, "thumb.rgba")] for t in times]
        theirs = [[FFMPEG, "-v", "error", "-y", "-ss", f"{t:.3f}", "-i", clip, "-frames:v", "1", "-vf", "scale='min(160,iw)':'min(160,ih)':force_original_aspect_ratio=decrease",
                   "-f", "image2pipe", "-vcodec", "png", os.path.join(scratch, "thumb.png")] for t in times]
        row(f"12 thumbnails, {name}", best([*ours]), best([*theirs]), "one program start per thumbnail on both sides")
        backend = subprocess.run([AMEDIA, "videoinfo", clip], capture_output=True, text=True).stdout.split("backend ")[-1].split()[0] if True else ""
        decode_ours = best([[AMEDIA, "decode", clip]], 2)
        decode_theirs = best([[FFMPEG, "-v", "error", "-i", clip, "-map", "0:v:0", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-"]], 2)
        row(f"decode to memory, {name}", decode_ours, decode_theirs, f"FFamp {backend}, FFmpeg software")
        if WINDOWS:
            row(f"decode to memory, {name}, FFmpeg on the GPU", decode_ours, best([[FFMPEG, "-v", "error", "-hwaccel", "d3d11va", "-i", clip, "-map", "0:v:0", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-"]], 2),
                f"FFamp {backend}, FFmpeg -hwaccel d3d11va")
        row(f"trim 20 s from 10 s, {name}", best([[AMEDIA, "trim", clip, os.path.join(scratch, "trim.mp4"), "--start", "10", "--end", "30"]], 5),
            best([[FFMPEG, "-y", "-v", "error", "-ss", "10", "-i", clip, "-t", "20", "-map", "0:v:0", "-map", "0:a:0?", "-c", "copy", "-avoid_negative_ts", "make_zero", "-movflags", "+faststart",
                   os.path.join(scratch, "trim_ff.mp4")]], 5))
        row(f"extract audio by copy, {name}", best([[AMEDIA, "extract-audio", clip, os.path.join(scratch, "sound.m4a")]], 5),
            best([[FFMPEG, "-y", "-v", "error", "-i", clip, "-vn", "-map", "0:a:0", "-c:a", "copy", "-movflags", "+faststart", os.path.join(scratch, "sound_ff.m4a")]], 5))
        row(f"extract audio to MP3 128 kbit/s, {name}", best([[AMEDIA, "extract-audio", clip, os.path.join(scratch, "sound.mp3"), "--mp3", "--bitrate", "128000"]], 3),
            best([[FFMPEG, "-y", "-v", "error", "-i", clip, "-vn", "-map", "0:a:0", "-c:a", "libmp3lame", "-b:a", "128k", os.path.join(scratch, "sound_ff.mp3")]], 3))
    if os.path.exists(raw) and os.path.exists(pcm):
        encoder = h264_encoder()
        ours = [AMEDIA, "encode", os.path.join(scratch, "export.mp4"), "--video", raw, "--size", "1920x1080", "--fps", "30", "--frames", "300", "--codec", "h264", "--bitrate", "8000000",
                "--audio", pcm, "--abitrate", "128000"]
        theirs = [FFMPEG, "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "30", "-i", raw, "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", pcm,
                  "-map", "0:v", "-map", "1:a", "-frames:v", "300", "-c:v", encoder, "-b:v", "8M", "-g", "30", "-pix_fmt", "yuv420p", "-c:a", aac_encoder(128), "-b:a", "128k",
                  "-shortest", "-movflags", "+faststart", os.path.join(scratch, "export_ff.mp4")]
        if encoder == "libx264":
            theirs[theirs.index("libx264") + 1:theirs.index("libx264") + 1] = ["-preset", "veryfast", "-profile:v", "high"]
        backend = subprocess.run([AMEDIA, "encode", os.path.join(scratch, "probe.mp4"), "--video", raw, "--size", "1920x1080", "--frames", "2", "--codec", "h264"], capture_output=True, text=True)
        note = (backend.stdout + backend.stderr).strip().splitlines()[-1][:80] if (backend.stdout + backend.stderr).strip() else ""
        row("export MP4, 300 frames 1080p H.264 8 Mbit/s + AAC 128 kbit/s", best([ours], 2), best([theirs], 2), f"FFmpeg {encoder}; FFamp: {note}")


def libraries(folder):
    F = [FFMPEG, "-nostdin", "-v", "error", "-y"]
    big = "testsrc2=size=1920x1080:rate=30,noise=alls=10:allf=t"
    made = {"vp9_1080_60s.webm": ["-c:v", "libvpx-vp9", "-deadline", "realtime", "-cpu-used", "8", "-row-mt", "1", "-b:v", "5M"],
            "av1_1080_60s.mp4": ["-c:v", "libsvtav1", "-preset", "10", "-b:v", "4M"]}
    for name, codec in made.items():
        if not os.path.exists(os.path.join(folder, name)):
            subprocess.run(F + ["-f", "lavfi", "-i", big, "-t", "60", *codec, "-g", "60", "-pix_fmt", "yuv420p", os.path.join(folder, name)], check=True)
    raw, scratch = os.path.join(folder, "raw_1080.yuv"), os.path.join(folder, "speed")
    os.makedirs(scratch, exist_ok=True)
    os.environ.update(FFAMP_NO_SYSTEM_ENCODER="1", FFAMP_NO_MEDIA_FOUNDATION="1")
    print("| Library task | FFamp | FFmpeg | FFamp faster by | Note |")
    print("|---|---|---|---|---|")
    for name in ("h264_1080_60s.mp4", "hevc_1080_60s.mp4", "vp9_1080_60s.webm", "av1_1080_60s.mp4"):
        clip = os.path.join(folder, name)
        if not os.path.exists(clip):
            continue
        ours = [AMEDIA, "decode", clip, "--software"]
        backend = subprocess.run([AMEDIA, "videoinfo", clip, "--software"], capture_output=True, text=True).stdout.split("backend ")[-1].split()[0]
        row(f"software decode to memory, {name}", best([ours], 2), best([[FFMPEG, "-v", "error", "-i", clip, "-map", "0:v:0", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-"]], 2), f"FFamp {backend}")
    theirs = {"h264": ["libx264", "-preset", "veryfast"], "hevc": ["libx265", "-preset", "fast", "-x265-params", "log-level=error"], "av1": ["libsvtav1", "-preset", "10"],
              "vp9": ["libvpx-vp9", "-deadline", "realtime", "-cpu-used", "8", "-row-mt", "1"]}
    if os.path.exists(raw):
        for codec, settings in theirs.items():
            out = os.path.join(scratch, f"lib_{codec}" + (".webm" if codec == "vp9" else ".mp4"))
            row(f"software encode, 300 frames 1080p {codec} 8 Mbit/s",
                best([[AMEDIA, "encode", out, "--video", raw, "--size", "1920x1080", "--fps", "30", "--frames", "300", "--codec", codec, "--bitrate", "8000000"]], 2),
                best([F + ["-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "30", "-i", raw, "-frames:v", "300", "-c:v", *settings, "-b:v", "8M", "-g", "30", out]], 2),
                f"FFmpeg {settings[0]}")


if __name__ == "__main__":
    libraries(sys.argv[1]) if "--libraries" in sys.argv else main(sys.argv[1])
