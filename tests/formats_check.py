import json
import os
import random
import subprocess
import sys

import numpy

HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.environ.get("AMEDIA", os.path.join(HERE, "..", "build", "ffamp"))
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
FFPROBE = os.environ.get("FFPROBE", "ffprobe")


def video_times(path):
    run = subprocess.run([AMEDIA, "probe", "--json", "--packets", path], capture_output=True, text=True)
    if run.returncode != 0:
        return None
    return [(p["pts_time"], p["flags"]) for p in json.loads(run.stdout)["packets"] if p["codec_type"] == "video"]


def times(a, b):
    x, y = video_times(a), video_times(b)
    print(f"{os.path.basename(a)} and {os.path.basename(b)}: {len(x or [])} and {len(y or [])} video packets, times and key flags {'equal' if x == y else 'different'}")
    return x is not None and len(x) > 0 and x == y


def layout(path, pan, channels, least):
    run = subprocess.run([AMEDIA, "adecode", path], capture_output=True)
    got = numpy.frombuffer(run.stdout, dtype=numpy.float32)
    out = subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-i", path, "-af", "pan=" + pan, "-f", "f32le", "-"], capture_output=True).stdout
    want = numpy.frombuffer(out, dtype=numpy.float32)
    if len(got) != len(want) or len(got) % channels or not len(got):
        print(f"{os.path.basename(path)}: {len(got)} values against {len(want)}")
        return False
    got, want = got.reshape(-1, channels).astype(numpy.float64), want.reshape(-1, channels).astype(numpy.float64)
    edge = 400
    worst = 300.0
    for c in range(channels):
        a, b = got[edge:-edge, c], want[edge:-edge, c]
        power, noise = numpy.mean(b ** 2), numpy.mean((a - b) ** 2)
        if power < 1e-12:
            worst = min(worst, 300.0 if numpy.abs(a).max() < 1e-4 else 0.0)
        elif noise > 0:
            worst = min(worst, 10 * numpy.log10(power / noise))
    print(f"{os.path.basename(path)} as {channels} channels against the judge's {pan}: lowest channel SNR {worst:.1f} dB")
    return worst >= least


def bgra(path, least):
    size = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height", "-of", "csv=p=0", path],
                          capture_output=True, text=True, check=True).stdout.split()[0].split(",")
    frame = int(size[0]) * int(size[1]) * 4
    got = subprocess.run([AMEDIA, "decode", path, "--bgra"], capture_output=True).stdout
    want = subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-i", path, "-map", "0:v:0", "-fps_mode", "passthrough", "-vf", "scale=flags=bicubic+full_chroma_int+accurate_rnd", "-pix_fmt", "bgra", "-f", "rawvideo", "-"],
                          capture_output=True).stdout
    count = min(len(got), len(want)) // frame
    worst = 99.0
    for n in range(count):
        a = numpy.frombuffer(got[n * frame:(n + 1) * frame], dtype=numpy.uint8).reshape(-1, 4)[:, :3].astype(numpy.float64)
        b = numpy.frombuffer(want[n * frame:(n + 1) * frame], dtype=numpy.uint8).reshape(-1, 4)[:, :3].astype(numpy.float64)
        mse = numpy.mean((a - b) ** 2)
        if mse > 0:
            worst = min(worst, 10 * numpy.log10(255 * 255 / mse))
    print(f"decode {os.path.basename(path)} as BGRA: frames {count}, judge {len(want) // frame}, lowest PSNR {worst:.2f} dB")
    return count > 0 and len(got) == len(want) and worst >= least


def late(path, count, seed, least):
    info = subprocess.run([AMEDIA, "videoinfo", path], capture_output=True, text=True, check=True).stdout.split()
    found = dict(zip(info[0::2], info[1::2]))
    width, height = (int(v) for v in found["size"].split("x"))
    frames = int(found["frames"])
    size = width * height * 3 // 2
    rng = random.Random(seed)
    picks = sorted(set([0, frames - 1] + [rng.randrange(frames) for _ in range(count)]))
    ours = subprocess.run([AMEDIA, "frames", path, "--list", ",".join(str(n) for n in picks)], capture_output=True).stdout
    worst = 99.0
    rate = 25.0
    for i, n in enumerate(picks):
        want = subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-ss", f"{n / rate:.3f}", "-i", path, "-frames:v", "1", "-pix_fmt", "yuvj420p", "-f", "rawvideo", "-"],
                              capture_output=True).stdout
        a = numpy.frombuffer(ours[i * size:(i + 1) * size], dtype=numpy.uint8).astype(numpy.float64)
        b = numpy.frombuffer(want, dtype=numpy.uint8).astype(numpy.float64)
        if len(a) != size or len(b) != size:
            worst = 0.0
            continue
        mse = numpy.mean((a - b) ** 2)
        if mse > 0:
            worst = min(worst, 10 * numpy.log10(255 * 255 / mse))
    judge = int(subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=nb_frames", "-of", "csv=p=0", path],
                               capture_output=True, text=True).stdout.strip().strip(",") or 0)
    print(f"{os.path.basename(path)}: {frames} frames, judge {judge}, {len(picks)} frames across {os.path.getsize(path) >> 20} MiB, lowest PSNR {worst:.2f} dB")
    return frames == judge and worst >= least


if __name__ == "__main__":
    kind, rest = sys.argv[1], sys.argv[2:]
    if kind == "times":
        ok = times(rest[0], rest[1])
    elif kind == "layout":
        ok = layout(rest[0], rest[1], int(rest[2]), float(rest[3]))
    elif kind == "bgra":
        ok = bgra(rest[0], float(rest[1]))
    elif kind == "late":
        ok = late(rest[0], int(rest[1]), int(rest[2]), float(rest[3]))
    else:
        ok = False
    sys.exit(0 if ok else 1)
