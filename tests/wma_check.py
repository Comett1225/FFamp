import os
import subprocess
import sys
import time

import numpy

from audio import AMEDIA, FFMPEG, describe, judge, ours


def frame_length(codec, rate):
    if rate <= 16000:
        return 512
    if rate <= 22050 or (rate <= 32000 and codec == "wma1"):
        return 1024
    return 2048


def track(path):
    lines = subprocess.run([AMEDIA, "packets", path], capture_output=True, text=True).stdout.splitlines()
    head = next(l.split() for l in lines if l.startswith("track") and " audio " in l)
    return lines[0].split()[1], head[3], int(head[7])


def hidden(path, rate):
    form, codec, start = track(path)
    return 0 if form == "mkv" and start > 0 else 2 * frame_length(codec, rate)


def against(path, least):
    got, rate, channels = ours(["adecode", path])
    if got is None:
        return False
    want = judge(path, ["-i", path, "-map", "0:a:0"], channels)
    skip = hidden(path, rate)
    quiet = not numpy.any(got[:skip])
    shift, ratio, _ = describe(f"{os.path.basename(path)} ({rate} Hz, {channels} ch) after {skip} silent frames", got[skip:], want)
    return len(got) - skip == len(want) and quiet and (shift == 0 or not numpy.any(want)) and ratio >= least


def same(first, second):
    a, rate, _ = ours(["adecode", first])
    b, _, _ = ours(["adecode", second])
    if a is None or b is None:
        return False
    x, y = a[hidden(first, rate):], b[hidden(second, rate):]
    print(f"{os.path.basename(first)} and {os.path.basename(second)}: {len(x)} and {len(y)} frames after the hidden start, identical {numpy.array_equal(x, y)}")
    return numpy.array_equal(x, y)


def best(command, runs=3):
    times = []
    for _ in range(runs):
        start = time.perf_counter()
        subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        times.append(time.perf_counter() - start)
    return min(times)


def speed(path, limit):
    mine = best([AMEDIA, "adecode", path])
    theirs = best([FFMPEG, "-nostdin", "-v", "error", "-threads", "1", "-i", path, "-map", "0:a:0", "-f", "f32le", "-"])
    print(f"{os.path.basename(path)}: ours {mine:.3f} s, the judge {theirs:.3f} s, ratio {mine / theirs:.2f} (limit {limit})")
    return mine <= theirs * limit


def seed(path, out):
    head = open(path, "rb").read(1 << 20)
    at = head.find(bytes.fromhex("409E69F84D5BCF11A8FD00805F5C442B"))
    if at < 0:
        return False
    form = head[at + 54:]
    form = form[:18 + int.from_bytes(form[16:18], "little")]
    lines = subprocess.run([AMEDIA, "packets", path], capture_output=True, text=True).stdout.splitlines()
    number = next(l.split()[1] for l in lines if l.startswith("track") and " audio " in l)
    packets = subprocess.run([AMEDIA, "packets", path, "--raw", "--track", number], capture_output=True).stdout
    open(out, "wb").write(form + packets)
    return len(packets) > 0


if __name__ == "__main__":
    kind, a = sys.argv[1], sys.argv[2:]
    if kind == "against":
        ok = against(a[0], float(a[1]))
    elif kind == "same":
        ok = same(a[0], a[1])
    elif kind == "seed":
        ok = seed(a[0], a[1])
    elif kind == "speed":
        ok = speed(a[0], float(a[1]))
    else:
        ok = False
    sys.exit(0 if ok else 1)
