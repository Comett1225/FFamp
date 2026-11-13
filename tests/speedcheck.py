import os
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.environ.get("AMEDIA", os.path.join(HERE, "..", "build", "ffamp"))


def make(path, codec, count, width=1920, height=1080):
    encoder = subprocess.Popen([AMEDIA, "encode", path, "--video", "-", "--size", f"{width}x{height}", "--fps", "30", "--codec", codec, "--bitrate", "6000000"],
                               stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    x = np.arange(width, dtype=np.float32)[None, :]
    y = np.arange(height, dtype=np.float32)[:, None]
    grain = ((np.arange(width)[None, :] * 7 + np.arange(height)[:, None] * 13) % 23).astype(np.float32)
    for n in range(count):
        luma = 126 + 90 * np.sin((x + 9 * n) / 41) * np.cos((y + 5 * n) / 57) + grain
        u = 128 + 60 * np.sin((x[:, ::2] + y[::2] + 6 * n) / 90)
        v = 128 + 60 * np.cos((x[:, ::2] - y[::2] + 4 * n) / 70)
        for plane in (luma, u, v):
            encoder.stdin.write(np.clip(plane, 16, 235).astype(np.uint8).tobytes())
    encoder.stdin.close()
    return encoder.wait() == 0 and os.path.getsize(path) > 0


def rate(count, minimum, command):
    best = None
    for _ in range(3):
        start = time.perf_counter()
        if subprocess.run([AMEDIA] + command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0:
            print("      the command failed: " + " ".join(command))
            return False
        spent = time.perf_counter() - start
        best = spent if best is None else min(best, spent)
    print(f"      {count / best:.0f} frames per second, whole program, best of 3 (at least {minimum:.0f} wanted)")
    return count / best >= minimum


def steps(path, extra):
    whole = subprocess.run([AMEDIA, "decode", path] + extra, capture_output=True).stdout
    one = len(subprocess.run([AMEDIA, "frames", path, "--list", "0"] + extra, capture_output=True).stdout)
    if not one or len(whole) % one:
        return False
    count = len(whole) // one
    if count < 30:
        return False
    half = count // 2
    order = list(range(count - 6, count)) + list(range(3, 16)) + [0, 1, 2] + list(range(half, half + 12)) + [1] + list(range(half - 2, half + 3)) + list(range(count - 14, count))
    got = subprocess.run([AMEDIA, "frames", path, "--list", ",".join(map(str, order))] + extra, capture_output=True).stdout
    wanted = b"".join(whole[n * one:(n + 1) * one] for n in order)
    print(f"      {len(order)} frames in runs after jumps, {'all equal to the straight decode' if got == wanted else 'DIFFERENT from the straight decode'}")
    return got == wanted


if __name__ == "__main__":
    kind = sys.argv[1]
    if kind == "make":
        ok = make(sys.argv[2], sys.argv[3], int(sys.argv[4]))
    elif kind == "rate":
        ok = rate(int(sys.argv[2]), float(sys.argv[3]), sys.argv[4:])
    else:
        ok = steps(sys.argv[2], sys.argv[3:])
    sys.exit(0 if ok else 1)
