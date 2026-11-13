import hashlib
import os
import random
import struct
import subprocess
import sys

import numpy

numpy.seterr(all="ignore")

HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.environ.get("AMEDIA", os.path.join(HERE, "..", "build", "amedia"))
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")


def signal(rate, channels, seconds):
    count = int(round(rate * seconds))
    t = numpy.arange(count, dtype=numpy.float64) / rate
    fade = numpy.minimum(1.0, numpy.minimum(t, t[-1] - t) / 0.02)
    out = numpy.zeros((count, channels))
    for c in range(channels):
        low = channels == 6 and c == 3
        a0, a1 = (30.0, 110.0) if low else (150.0 + 70 * c, 3500.0 + 150 * c)
        b0, b1 = (90.0, 40.0) if low else (5000.0 - 200 * c, 400.0 + 50 * c)
        up = numpy.sin(2 * numpy.pi * (a0 * t + (a1 - a0) * t * t / (2 * seconds)))
        down = numpy.sin(2 * numpy.pi * (b0 * t + (b1 - b0) * t * t / (2 * seconds)))
        out[:, c] = (0.3 * up + 0.2 * down) * fade
    return out


def whole16(x):
    return numpy.round(x * 32767).astype(numpy.int16)


def write_wav(path, x, rate, kind="s16"):
    channels = x.shape[1]
    if kind == "s16":
        data, tag, bits = whole16(x).tobytes(), 1, 16
    elif kind == "s24":
        v = numpy.round(x * 8388607).astype("<i4")
        data, tag, bits = v.view(numpy.uint8).reshape(-1, 4)[:, :3].tobytes(), 1, 24
    elif kind == "s32":
        data, tag, bits = numpy.round(x * 2147483647).astype("<i4").tobytes(), 1, 32
    elif kind == "u8":
        data, tag, bits = (numpy.round(x * 127) + 128).astype(numpy.uint8).tobytes(), 1, 8
    elif kind == "f32":
        data, tag, bits = x.astype("<f4").tobytes(), 3, 32
    else:
        data, tag, bits = x.astype("<f8").tobytes(), 3, 64
    block = channels * bits // 8
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, tag, channels, rate, rate * block, block, bits))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def ours(args):
    run = subprocess.run([AMEDIA] + args, capture_output=True)
    note = run.stderr.decode().strip()
    if run.returncode != 0:
        print("amedia failed:", note)
        return None, 0, 0
    parts = note.split(",")
    rate, channels = int(parts[0].split()[0]), int(parts[1].split()[0])
    return numpy.frombuffer(run.stdout, dtype=numpy.float32).reshape(-1, channels), rate, channels


def judge(path, extra, channels, kind="f32le"):
    out = subprocess.run([FFMPEG, "-nostdin", "-v", "error"] + extra + ["-f", kind, "-"], capture_output=True).stdout
    return numpy.frombuffer(out, dtype=numpy.float32 if kind == "f32le" else numpy.int16).reshape(-1, channels)


def lag(a, b, span=4000):
    n = min(len(a), len(b))
    if n < 16:
        return 0
    fa = numpy.fft.rfft(a[:n].astype(numpy.float64), 2 * n)
    fb = numpy.fft.rfft(b[:n].astype(numpy.float64), 2 * n)
    c = numpy.fft.irfft(fa * numpy.conj(fb))
    span = min(span, n - 1)
    c = numpy.concatenate([c[-span:], c[:span + 1]])
    return int(numpy.argmax(c)) - span


def snr(got, want):
    noise = numpy.mean((got.astype(numpy.float64) - want.astype(numpy.float64)) ** 2)
    power = numpy.mean(want.astype(numpy.float64) ** 2)
    return 300.0 if noise == 0 else 10 * numpy.log10(max(power, 1e-30) / noise)


def worst_lag(got, want):
    lags = [lag(got[:, c], want[:, c]) for c in range(got.shape[1]) if not (got.shape[1] == 6 and c == 3)]
    return max(lags, key=abs)


def describe(name, got, want, edge=0):
    n = min(len(got), len(want))
    a, b = got[edge:n - edge if edge else n], want[edge:n - edge if edge else n]
    shift = worst_lag(got[:n], want[:n])
    ratio = snr(a, b)
    peak = float(numpy.abs(a.astype(numpy.float64) - b).max()) if len(a) else 0.0
    same = len(got) == len(want) and numpy.array_equal(got, want)
    print(f"{name}: frames {len(got)} against {len(want)}, offset {shift}, SNR {ratio:.1f} dB, largest difference {peak:.2e}, identical {same}")
    return shift, ratio, same


def truth(path, rate, channels, seconds, least, slack, extra):
    got, r, c = ours(["adecode", path] + extra)
    if got is None:
        return False
    want = whole16(signal(rate, channels, seconds)).astype(numpy.float32) / 32768
    if (r, c) != (rate, channels):
        print(f"{os.path.basename(path)}: {r} Hz {c} channels, expected {rate} Hz {channels} channels")
        return False
    shift, ratio, _ = describe(os.path.basename(path) + " against its source signal", got, want)
    return len(got) == len(want) and abs(shift) <= slack and ratio >= least


def against(path, least, tail, late, exact, decoder):
    got, rate, channels = ours(["adecode", path])
    if got is None:
        return False
    want = judge(path, decoder + ["-i", path, "-map", "0:a:0"], channels)
    total = len(want)
    name = os.path.basename(path) + " against the judge" + (" with its own decoder delay of %d frames taken off" % late if late else "")
    shift, ratio, same = describe(name, got[:len(got) - late], want[late:])
    return total - len(got) == tail and shift == 0 and ratio >= least and (same or not exact)


def checksum(path, listing):
    run = subprocess.run([AMEDIA, "adecode", path], capture_output=True)
    digest = hashlib.md5(run.stdout).hexdigest()
    name = os.path.basename(path)
    for line in open(listing):
        if line.split()[1:] == [name]:
            print(f"{name}: decoded checksum {digest}")
            return line.split()[0] == digest
    print(f"{digest}  {name}")
    return False


def chunks(command, path, extra, count, seed):
    whole, rate, channels = ours([command, path] + extra)
    if whole is None:
        return False
    rng = random.Random(seed)
    total = len(whole)
    bad = 0
    for size in (1, 333, 4097):
        part, _, _ = ours([command, path] + extra + ["--chunk", str(size), "--frames", str(min(total, 3000 if size == 1 else total))])
        bad += not numpy.array_equal(part, whole[:len(part)])
    picks = [(0, 100), (total - 100, 300), (-50, 100)] + [(rng.randrange(-200, total + 200), rng.randrange(1, 5000)) for _ in range(count)]
    part, _, _ = ours([command, path] + extra + ["--list", ",".join(f"{start}:{frames}" for start, frames in picks)])
    at = 0
    for start, frames in picks:
        want = numpy.zeros((frames, channels), dtype=numpy.float32)
        a, b = max(start, 0), min(start + frames, total)
        if b > a:
            want[a - start:b - start] = whole[a:b]
        bad += part is None or not numpy.array_equal(part[at:at + frames], want)
        at += frames
    print(f"{os.path.basename(path)} {command} {' '.join(extra)}: {len(picks)} random reads and 3 chunk sizes over {total} frames, {bad} different")
    return bad == 0


def sweep(rate, seconds, top):
    t = numpy.arange(int(rate * seconds), dtype=numpy.float64) / rate
    return 0.5 * numpy.sin(2 * numpy.pi * (20 * t + (top - 20) * t * t / (2 * seconds)))


def resample(source, target, least, chunk):
    seconds = 4.0
    top = 0.45 * min(source, target)
    x = sweep(source, seconds, top).astype(numpy.float32)
    run = subprocess.run([AMEDIA, "resample", "--from", str(source), "--to", str(target), "--chunk", str(chunk)], input=x.tobytes(), capture_output=True)
    got = numpy.frombuffer(run.stdout, dtype=numpy.float32).astype(numpy.float64)
    want = sweep(target, seconds, top)
    expected = -(-len(x) * target // source)
    n = min(len(got), len(want))
    edge = target // 20
    ratio = snr(got[edge:n - edge], want[edge:n - edge])
    print(f"resample {source} to {target} Hz, sweep 20 Hz to {top:.0f} Hz: {len(got)} frames (expected {expected}), SNR {ratio:.1f} dB against the exact sweep")
    return len(got) == expected and ratio >= least


def streaming(source, target, channels, out_channels):
    rng = numpy.random.RandomState(5)
    x = (rng.rand(30000, channels).astype(numpy.float32) - 0.5)
    base = [AMEDIA, "resample", "--from", str(source), "--to", str(target), "--channels", str(channels), "--to-channels", str(out_channels)]
    whole = subprocess.run(base, input=x.tobytes(), capture_output=True).stdout
    bad = sum(subprocess.run(base + ["--chunk", str(size)], input=x.tobytes(), capture_output=True).stdout != whole for size in (1, 7, 1000))
    print(f"streaming {source} to {target} Hz, {channels} to {out_channels} channels: {len(whole) // (4 * out_channels)} frames, {bad} of 3 push sizes differ")
    return bad == 0 and len(whole) > 0


def alias(source, target):
    tone = 0.53 * target
    t = numpy.arange(source * 2, dtype=numpy.float64) / source
    x = (0.5 * numpy.sin(2 * numpy.pi * tone * t)).astype(numpy.float32)
    run = subprocess.run([AMEDIA, "resample", "--from", str(source), "--to", str(target)], input=x.tobytes(), capture_output=True)
    got = numpy.frombuffer(run.stdout, dtype=numpy.float32).astype(numpy.float64)
    edge = target // 10
    level = 10 * numpy.log10(max(numpy.mean(got[edge:-edge] ** 2), 1e-30) / 0.125)
    print(f"resample {source} to {target} Hz, tone at {tone:.0f} Hz above the new limit: left over at {level:.1f} dB")
    return level < -100


def fuzz(folder, rounds, seed):
    rng = random.Random(seed)
    names = sorted(n for n in os.listdir(folder) if n.endswith((".m4a", ".mp4", ".mov")))
    scratch = os.path.join(HERE, "out", f"fuzz_{os.getpid()}.bin")
    crashed = hung = accepted = 0
    for name in names:
        data = open(os.path.join(folder, name), "rb").read()
        for i in range(rounds):
            b = bytearray(data)
            kind = i % 4
            if kind == 0:
                b = b[:rng.randrange(8, len(b))]
            elif kind == 1:
                for _ in range(rng.randrange(1, 6)):
                    b[rng.randrange(len(b))] = rng.randrange(256)
            elif kind == 2:
                moov = data.find(b"moov")
                for _ in range(rng.randrange(1, 8)):
                    b[moov + rng.randrange(min(1200, len(b) - moov))] = rng.choice([0, 1, 0x7F, 0x80, 0xFF, rng.randrange(256)])
            else:
                at = rng.randrange(len(b) - 4)
                b[at:at + 4] = rng.choice([b"\xff\xff\xff\xff", b"\x00\x00\x00\x00", b"\x7f\xff\xff\xff", b"\x80\x00\x00\x00"])
            open(scratch, "wb").write(b)
            for command in (["adecode", scratch], ["pcm", scratch, "--rate", "32000", "--channels", "2", "--list", "0:5000,-100:300,1000000:100,30000:2000"]):
                try:
                    code = subprocess.run([AMEDIA] + command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=20).returncode
                except subprocess.TimeoutExpired:
                    hung += 1
                    print("hung:", name, i, command[0])
                    os.replace(scratch, os.path.join(HERE, "out", f"hung_{name}_{i}.bin"))
                    break
                accepted += code == 0
                if code not in (0, 1):
                    crashed += 1
                    print("crashed:", name, i, command[0], code)
                    open(os.path.join(HERE, "out", f"crash_{name}_{i}.bin"), "wb").write(b)
    if os.path.exists(scratch):
        os.remove(scratch)
    print(f"broken files: {len(names) * rounds} damaged copies of {len(names)} fixtures, {crashed} crashes, {hung} hangs, {accepted} of {2 * len(names) * rounds} runs still gave sound")
    return crashed == 0 and hung == 0


def mixing(path, rate, channels):
    native, native_rate, native_channels = ours(["adecode", path])
    got, _, _ = ours(["pcm", path, "--rate", str(native_rate), "--channels", str(channels)])
    side = numpy.sqrt(0.5)
    rows = {
        (1, 2): [[side], [side]],
        (2, 1): [[side, side]],
        (6, 2): [[1, 0, side, 0, side, 0], [0, 1, side, 0, 0, side]],
        (6, 1): [[side, side, 1, 0, 0.5, 0.5]],
        (8, 2): [[1, 0, side, 0, side, 0, side, 0], [0, 1, side, 0, 0, side, 0, side]],
    }[(native_channels, channels)]
    want = native.astype(numpy.float64) @ numpy.array(rows).T
    ratio = snr(got, want)
    print(f"{os.path.basename(path)} mixed from {native_channels} to {channels} channels: SNR {ratio:.1f} dB against the expected levels")
    return len(got) == len(want) and ratio > 120


def reader(path, rate, channels, kind, least, slack, start):
    native, native_rate, native_channels = ours(["adecode", path])
    got = subprocess.run([AMEDIA, "pcm", path, "--rate", str(rate), "--channels", str(channels), "--format", kind, "--start", str(start)], capture_output=True).stdout
    got = numpy.frombuffer(got, dtype=numpy.float32 if kind == "f32le" else numpy.int16).reshape(-1, channels)
    want = judge(path, (["-ss", str(start)] if start else []) + ["-i", path, "-map", "0:a:0", "-ar", str(rate), "-ac", str(channels)], channels, kind)
    edge = rate // 50
    name = f"{os.path.basename(path)} as {rate} Hz {channels} channels {kind}" + (f" from {start} s" if start else "")
    shift, ratio, same = describe(name, got, want, edge)
    return abs(len(got) - len(want)) <= slack and shift == 0 and ratio >= least


if __name__ == "__main__":
    kind = sys.argv[1]
    a = sys.argv[2:]
    if kind == "signal":
        write_wav(a[3], signal(int(a[0]), int(a[1]), float(a[2])), int(a[0]), a[4] if len(a) > 4 else "s16")
        ok = True
    elif kind == "truth":
        ok = truth(a[0], int(a[1]), int(a[2]), float(a[3]), float(a[4]), int(a[5]), a[6:])
    elif kind == "against":
        ok = against(a[0], float(a[1]), int(a[2]), int(a[3]), "exact" in a[4:], ["-c:a", "libopus"] if "libopus" in a[4:] else [])
    elif kind == "checksum":
        ok = checksum(a[0], a[1])
    elif kind == "chunks":
        ok = chunks(a[0], a[1], a[4:], int(a[2]), int(a[3]))
    elif kind == "resample":
        ok = resample(int(a[0]), int(a[1]), float(a[2]), int(a[3]) if len(a) > 3 else 8192)
    elif kind == "streaming":
        ok = streaming(int(a[0]), int(a[1]), int(a[2]), int(a[3]))
    elif kind == "alias":
        ok = alias(int(a[0]), int(a[1]))
    elif kind == "fuzz":
        ok = fuzz(a[0], int(a[1]), int(a[2]))
    elif kind == "mixing":
        ok = mixing(a[0], 0, int(a[1]))
    elif kind == "reader":
        ok = reader(a[0], int(a[1]), int(a[2]), a[3], float(a[4]), int(a[5]), float(a[6]) if len(a) > 6 else 0)
    else:
        ok = False
    sys.exit(0 if ok else 1)
