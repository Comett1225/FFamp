import os
import subprocess
import sys

import numpy

from audio import AMEDIA, FFMPEG, describe, judge, ours

numpy.seterr(all="ignore")


def app_mix(path, rate, channels, least, slack):
    got, _, _ = ours(["pcm", path, "--rate", str(rate), "--channels", str(channels)])
    if got is None:
        return False
    want = judge(path, ["-i", path, "-map", "0:a:0", "-af", "aresample=async=1:first_pts=0", "-ar", str(rate), "-ac", str(channels)], channels)
    peak, top = float(numpy.abs(got).max()) if len(got) else 0.0, float(numpy.abs(want).max()) if len(want) else 0.0
    print(f"peaks {peak:.4f} against {top:.4f}")
    shift, ratio, _ = describe(f"{os.path.basename(path)} as the app reads it, {rate} Hz {channels} channels", got, want)
    return abs(len(got) - len(want)) <= slack and shift == 0 and ratio >= least


def playback(path, rate, channels, least):
    run = subprocess.run([AMEDIA, "pcm", path, "--rate", str(rate), "--channels", str(channels), "--format", "s16le"], capture_output=True)
    got = numpy.frombuffer(run.stdout, dtype=numpy.int16).reshape(-1, channels).astype(numpy.float32) / 32768
    want = judge(path, ["-i", path, "-map", "0:a:0", "-vn", "-ar", str(rate), "-ac", str(channels)], channels, "s16le").astype(numpy.float32) / 32768
    clipped = int(numpy.sum(numpy.abs(got) >= 32767 / 32768))
    print(f"peaks {float(numpy.abs(got).max()):.4f} against {float(numpy.abs(want).max()):.4f}, {clipped} samples at full scale")
    shift, ratio, _ = describe(f"{os.path.basename(path)} as the viewer plays it, 16-bit", got, want)
    return len(got) == len(want) and shift == 0 and ratio >= least


def bounded(path, frames):
    got, rate, channels = ours(["adecode", path])
    if got is None:
        return False
    peak = float(numpy.abs(got).max()) if len(got) else 0.0
    print(f"{os.path.basename(path)}: {len(got)} frames (expected {frames}), largest value {peak:.4g}")
    return len(got) == frames and peak <= 1.0


def damage(source, target, seed, count):
    import random
    r = random.Random(seed)
    data = bytearray(open(source, "rb").read())
    middle = len(data) // 2
    data[middle:middle + count] = bytes(r.randrange(256) for _ in range(count))
    open(target, "wb").write(data)
    return True


def frames_of(path):
    run = subprocess.run([AMEDIA, "adecode", path, "--frames", "0"], capture_output=True)
    print(run.stderr.decode().strip().split(",")[-1].split()[0])
    return True


if __name__ == "__main__":
    kind, a = sys.argv[1], sys.argv[2:]
    if kind == "app":
        ok = app_mix(a[0], int(a[1]), int(a[2]), float(a[3]), int(a[4]) if len(a) > 4 else 0)
    elif kind == "playback":
        ok = playback(a[0], int(a[1]), int(a[2]), float(a[3]))
    elif kind == "bounded":
        ok = bounded(a[0], int(a[1]))
    elif kind == "damage":
        ok = damage(a[0], a[1], int(a[2]), int(a[3]))
    elif kind == "frames":
        ok = frames_of(a[0])
    else:
        ok = False
    sys.exit(0 if ok else 1)
