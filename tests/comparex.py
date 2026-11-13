import hashlib
import os
import random
import subprocess
import sys
import time

import numpy

HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.environ.get("AMEDIA", os.path.join(HERE, "..", "build", "amedia"))
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")


def info(path, extra):
    words = subprocess.run([AMEDIA, "videoinfo", path] + extra, capture_output=True, text=True, check=True).stdout.split()
    found = dict(zip(words[0::2], words[1::2]))
    width, height = (int(v) for v in found["size"].split("x"))
    chroma_width, chroma_height = (int(v) for v in found["chroma"].split("x"))
    found.update(width=width, height=height, depth=int(found["bits"]), count=int(found["frames"]), chroma_width=chroma_width, chroma_height=chroma_height)
    layout = "444" if chroma_width == width else "420" if chroma_height < height else "422"
    found["pix_fmt"] = "yuv" + layout + ("p" if found["depth"] == 8 else "p10le")
    found["frame_bytes"] = (width * height + 2 * chroma_width * chroma_height) * (1 if found["depth"] == 8 else 2)
    return found


def reference(path, pix_fmt, more=()):
    return subprocess.Popen([FFMPEG, "-nostdin", "-v", "error", "-noautorotate", "-i", path, "-map", "0:v:0", "-fps_mode", "passthrough", *more,
                             "-pix_fmt", pix_fmt, "-f", "rawvideo", "-"], stdout=subprocess.PIPE)


def psnr(a, b, depth):
    kind = numpy.uint8 if depth == 8 else numpy.dtype("<u2")
    x = numpy.frombuffer(a, dtype=kind).astype(numpy.float64)
    y = numpy.frombuffer(b, dtype=kind).astype(numpy.float64)
    mse = numpy.mean((x - y) ** 2)
    top = (1 << depth) - 1
    return 99.0 if mse == 0 else 10 * numpy.log10(top * top / mse)


def read(pipe, count):
    data = pipe.read(count)
    return data if len(data) == count else None


def decode(path, floor, extra):
    shape = info(path, extra)
    size = shape["frame_bytes"]
    ref = reference(path, shape["pix_fmt"])
    ours = subprocess.Popen([AMEDIA, "decode", path] + extra, stdout=subprocess.PIPE)
    total = same = 0
    worst = 99.0
    extra_frames = False
    while True:
        a = read(ref.stdout, size)
        b = read(ours.stdout, size)
        if a is None or b is None:
            extra_frames = (a is not None) or (b is not None)
            break
        total += 1
        if a == b:
            same += 1
        else:
            worst = min(worst, psnr(a, b, shape["depth"]))
    ref.kill()
    ours.kill()
    print(f"decode {os.path.basename(path)} [{shape['backend']}, {shape['pix_fmt']}]: frames {total}, identical {same}, lowest PSNR {worst:.2f} dB, count mismatch {extra_frames}")
    if floor > 0:
        return total > 0 and worst >= floor and not extra_frames
    return total > 0 and same == total and not extra_frames


def seek(path, count, seed, extra):
    shape = info(path, extra)
    size = shape["frame_bytes"]
    frames = shape["count"]
    rng = random.Random(seed)
    picks = [rng.randrange(frames) for _ in range(count - 2)] + [0, frames - 1]
    rng.shuffle(picks)
    ref = reference(path, shape["pix_fmt"])
    truth = {}
    for n in range(frames):
        data = read(ref.stdout, size)
        if data is None:
            break
        truth[n] = data
    ref.kill()
    run = subprocess.run([AMEDIA, "frames", path, "--list", ",".join(str(n) for n in picks)] + extra, capture_output=True)
    exact = sum(1 for i, n in enumerate(picks) if n in truth and run.stdout[i * size:(i + 1) * size] == truth[n])
    summary = run.stderr.decode().strip().splitlines()[-1] if run.stderr else ""
    print(f"seek {os.path.basename(path)} [{shape['backend']}]: {exact}/{len(picks)} random frames exact; {summary}")
    return exact == len(picks)


def app_preview(rgba, hlg):
    n = numpy.arange(4096, dtype=numpy.float32) / numpy.float32(4095)
    if hlg:
        levels = numpy.where(n <= 0.5, n * n / 3, (numpy.exp((n - numpy.float32(0.55991073)) / numpy.float32(0.17883277)) + numpy.float32(0.28466892)) / 12)
    else:
        p = numpy.power(n, numpy.float32(1 / 78.84375))
        levels = numpy.power(numpy.maximum(p - numpy.float32(0.8359375), 0) / (numpy.float32(18.8515625) - numpy.float32(18.6875) * p), numpy.float32(1 / 0.1593017578125)) * 10000 / 203
    levels = levels.astype(numpy.float32)
    encode = (numpy.where(n <= 0.0031308, 12.92 * n, 1.055 * numpy.power(n, 1 / 2.4) - 0.055) * 255.0 + 0.5).astype(numpy.float32)
    r, g, b = (levels[rgba[..., k] >> 4] for k in range(3))
    if hlg:
        gain = numpy.float32(4.926) * numpy.power(numpy.maximum(0.2627 * r + 0.678 * g + 0.0593 * b, 1e-6), 0.2).astype(numpy.float32)
        r, g, b = r * gain, g * gain, b * gain
    c = numpy.stack([1.6605 * r - 0.5876 * g - 0.0728 * b, -0.1246 * r + 1.1329 * g - 0.0083 * b, -0.0182 * r - 0.1006 * g + 1.1187 * b], axis=-1).astype(numpy.float32)
    peak = numpy.maximum(c.max(axis=-1), 1e-6)
    knee = numpy.where(peak <= 0.8, peak, 0.8 + 0.2 * (peak - 0.8) / ((peak - 0.8) + 0.2))
    scaled = numpy.clip(c * (knee / peak)[..., None], 0, 1)
    return encode[(scaled * 4095.0 + 0.5).astype(numpy.int32)].astype(numpy.uint8)


def sdr(path, mean_limit, share_limit):
    shape = info(path, [])
    width, height = shape["width"], shape["height"]
    ref = reference(path, "bgra", ("-sws_flags", "bilinear+accurate_rnd+full_chroma_int"))
    ours = subprocess.Popen([AMEDIA, "decode", path, "--bgra"], stdout=subprocess.PIPE)
    total = worst = 0
    mean = near = 0.0
    while True:
        a = read(ref.stdout, width * height * 4)
        b = read(ours.stdout, width * height * 4)
        if a is None or b is None:
            break
        gap = numpy.abs(numpy.frombuffer(a, dtype=numpy.uint8).astype(numpy.int32) - numpy.frombuffer(b, dtype=numpy.uint8).astype(numpy.int32))
        total += 1
        worst = max(worst, int(gap.max()))
        mean += float(gap.mean())
        near += float((gap <= 2).mean())
    ref.kill()
    ours.kill()
    mean /= max(total, 1)
    near /= max(total, 1)
    print(f"sdr {os.path.basename(path)}: frames {total}, mean difference {mean:.3f} of 255, within 2 code values {near * 100:.2f} %, largest {worst}")
    return total > 0 and mean <= mean_limit and near >= share_limit


def hdr(path, mean_limit, share_limit):
    shape = info(path, [])
    width, height = shape["width"], shape["height"]
    hlg = shape["transfer"] == "18"
    ref = reference(path, "rgba64le", ("-vf", "scale=flags=bilinear+accurate_rnd,format=yuv444p16le,scale=flags=bilinear+accurate_rnd"))
    ours = subprocess.Popen([AMEDIA, "decode", path, "--bgra"], stdout=subprocess.PIPE)
    total = 0
    worst = 0
    mean = 0.0
    near = 0.0
    while True:
        a = read(ref.stdout, width * height * 8)
        b = read(ours.stdout, width * height * 4)
        if a is None or b is None:
            break
        want = app_preview(numpy.frombuffer(a, dtype="<u2").reshape(height, width, 4), hlg).astype(numpy.int32)
        got = numpy.frombuffer(b, dtype=numpy.uint8).reshape(height, width, 4)[..., [2, 1, 0]].astype(numpy.int32)
        gap = numpy.abs(want - got)
        total += 1
        worst = max(worst, int(gap.max()))
        mean += float(gap.mean())
        near += float((gap <= 2).mean())
    ref.kill()
    ours.kill()
    mean /= max(total, 1)
    near /= max(total, 1)
    print(f"hdr {os.path.basename(path)} [{'HLG' if hlg else 'PQ'}]: frames {total}, mean difference {mean:.3f} of 255, within 2 code values {near * 100:.2f} %, largest {worst}")
    return total > 0 and mean <= mean_limit and near >= share_limit


def frames_of(command, size):
    run = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    while True:
        data = read(run.stdout, size)
        if data is None:
            break
        yield data
    run.kill()
    run.wait()


def quality(encoded, source, width, height, floor, with_ffmpeg):
    size = width * height * 3 // 2
    luma = width * height
    runs = [("own decoder", [AMEDIA, "decode", encoded]), ("own software decoder", [AMEDIA, "decode", encoded, "--software"])]
    if with_ffmpeg:
        runs.append(("ffmpeg", [FFMPEG, "-nostdin", "-v", "error", "-i", encoded, "-fps_mode", "passthrough", "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"]))
    results = []
    digests = []
    counts = []
    for name, command in runs:
        digest = hashlib.md5()
        error = 0.0
        count = 0
        with open(source, "rb") as original:
            for got in frames_of(command, size):
                want = original.read(size)
                if len(want) != size:
                    break
                digest.update(got)
                a = numpy.frombuffer(want, dtype=numpy.uint8, count=luma).astype(numpy.float64)
                b = numpy.frombuffer(got, dtype=numpy.uint8, count=luma).astype(numpy.float64)
                error += float(numpy.mean((a - b) ** 2))
                count += 1
        counts.append(count)
        digests.append(digest.hexdigest())
        mse = error / max(count, 1)
        results.append((name, 99.0 if mse == 0 else 10 * numpy.log10(255 * 255 / mse)))
    same = all(d == digests[0] for d in digests) and all(c == counts[0] for c in counts)
    kbit = os.path.getsize(encoded) * 8 / 1000
    print(f"quality {os.path.basename(encoded)}: {counts[0]} frames, {kbit:.0f} kbit, luma PSNR " + ", ".join(f"{n} {v:.2f} dB" for n, v in results) + f", decoders agree {same}")
    return counts[0] > 0 and same and all(v >= floor for _, v in results)


def luma_psnr(encoded, source, width, height):
    size = width * height * 3 // 2
    luma = width * height
    error = 0.0
    count = 0
    with open(source, "rb") as original:
        for got in frames_of([AMEDIA, "decode", encoded], size):
            want = original.read(size)
            if len(want) != size:
                break
            a = numpy.frombuffer(want, dtype=numpy.uint8, count=luma).astype(numpy.float64)
            b = numpy.frombuffer(got, dtype=numpy.uint8, count=luma).astype(numpy.float64)
            error += float(numpy.mean((a - b) ** 2))
            count += 1
    mse = error / max(count, 1)
    return count, 99.0 if mse == 0 else 10 * numpy.log10(255 * 255 / mse)


def race(raw, folder, frames):
    bitrate = 6000000
    rows = []
    for codec, theirs in (("av1", ["-c:v", "libsvtav1", "-preset", "10"]), ("vp9", ["-c:v", "libvpx-vp9", "-deadline", "realtime", "-cpu-used", "8", "-row-mt", "1"])):
        ours = os.path.join(folder, f"vx_race_{codec}.ivf")
        other = os.path.join(folder, f"vx_race_ffmpeg_{codec}.ivf")
        start = time.time()
        subprocess.run([AMEDIA, "encode", ours, "--video", raw, "--size", "1920x1080", "--fps", "30", "--codec", codec, "--bitrate", str(bitrate), "--frames", str(frames)],
                       stderr=subprocess.DEVNULL, check=True)
        own_time = time.time() - start
        start = time.time()
        subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "30", "-i", raw, "-frames:v", str(frames),
                        *theirs, "-b:v", str(bitrate), "-g", "30", other], env=dict(os.environ, SVT_LOG="1"), check=True)
        their_time = time.time() - start
        for name, path, spent in (("absent-media", ours, own_time), ("ffmpeg", other, their_time)):
            count, level = luma_psnr(path, raw, 1920, 1080)
            rows.append(f"race {codec} 1080p {name}: {count} frames in {spent:.2f} s ({count / spent:.1f} fps), {os.path.getsize(path) * 8 / (count / 30) / 1000:.0f} kbit/s for a {bitrate // 1000} kbit/s target, luma PSNR {level:.2f} dB")
    print("\n".join(rows))
    return True


def near(ours, other, source, width, height, slack):
    a = luma_psnr(ours, source, width, height)
    b = luma_psnr(other, source, width, height)
    return a[0] > 0 and a[0] == b[0] and a[1] >= b[1] - slack


def self_seek(path, count, seed, extra):
    shape = info(path, extra)
    size = shape["frame_bytes"]
    whole = subprocess.run([AMEDIA, "decode", path] + extra, capture_output=True).stdout
    frames = len(whole) // size
    rng = random.Random(seed)
    picks = [rng.randrange(frames) for _ in range(count - 2)] + [0, frames - 1]
    rng.shuffle(picks)
    run = subprocess.run([AMEDIA, "frames", path, "--list", ",".join(str(n) for n in picks)] + extra, capture_output=True)
    exact = sum(1 for i, n in enumerate(picks) if run.stdout[i * size:(i + 1) * size] == whole[n * size:(n + 1) * size])
    print(f"self seek {os.path.basename(path)} [{shape['backend']}]: {exact}/{len(picks)} random frames equal the straight decode of {frames} frames")
    return frames == shape["count"] and exact == len(picks)


def upsampled(plane, width, height):
    tall = plane
    if plane.shape[0] < height:
        above = numpy.vstack([plane[:1], plane[:-1]])
        below = numpy.vstack([plane[1:], plane[-1:]])
        tall = numpy.empty((plane.shape[0] * 2, plane.shape[1]), dtype=numpy.float32)
        tall[0::2] = 0.75 * plane + 0.25 * above
        tall[1::2] = 0.75 * plane + 0.25 * below
        tall = tall[:height]
    if tall.shape[1] == width:
        return tall
    left = numpy.hstack([tall[:, :1], tall[:, :-1]])
    right = numpy.hstack([tall[:, 1:], tall[:, -1:]])
    wide = numpy.empty((tall.shape[0], tall.shape[1] * 2), dtype=numpy.float32)
    wide[:, 0::2] = 0.75 * tall + 0.25 * left
    wide[:, 1::2] = 0.75 * tall + 0.25 * right
    return wide[:, :width]


def self_bgra(path, share_limit):
    shape = info(path, [])
    width, height, depth = shape["width"], shape["height"], shape["depth"]
    cw, ch = shape["chroma_width"], shape["chroma_height"]
    transfer, matrix = int(shape["transfer"]), int(shape["matrix"])
    hdr_kind = transfer in (16, 18)
    if matrix not in (1, 5, 6, 9, 10):
        matrix = 9 if hdr_kind else 1 if height >= 720 else 6
    kr, kb = (0.2126, 0.0722) if matrix == 1 else (0.2627, 0.0593) if matrix >= 9 else (0.299, 0.114)
    kind = numpy.uint8 if depth == 8 else numpy.dtype("<u2")
    unit = 1 << (depth - 8)
    full = shape["range"] == "full"
    top = (1 << depth) - 1
    planar = frames_of([AMEDIA, "decode", path], shape["frame_bytes"])
    packed = frames_of([AMEDIA, "decode", path, "--bgra"], width * height * 4)
    total = worst = 0
    near = 0.0
    for a, b in zip(planar, packed):
        values = numpy.frombuffer(a, dtype=kind).astype(numpy.float32)
        y = values[:width * height].reshape(height, width)
        u = upsampled(values[width * height:width * height + cw * ch].reshape(ch, cw), width, height)
        v = upsampled(values[width * height + cw * ch:].reshape(ch, cw), width, height)
        y = y / top if full else (y - 16 * unit) / (219 * unit)
        u = (u - 128 * unit) / (top if full else 224 * unit)
        v = (v - 128 * unit) / (top if full else 224 * unit)
        r = y + 2 * (1 - kr) * v
        bl = y + 2 * (1 - kb) * u
        g = (y - kr * r - kb * bl) / (1 - kr - kb)
        rgb = numpy.clip(numpy.stack([r, g, bl], axis=-1), 0, 1)
        if hdr_kind:
            want = app_preview((rgb * 65535 + 0.5).astype(numpy.uint16), transfer == 18).astype(numpy.int32)
        else:
            want = (rgb * 255 + 0.5).astype(numpy.int32)
        got = numpy.frombuffer(b, dtype=numpy.uint8).reshape(height, width, 4)[..., [2, 1, 0]].astype(numpy.int32)
        gap = numpy.abs(want - got)
        total += 1
        worst = max(worst, int(gap.max()))
        near += float((gap <= 1).mean())
    near /= max(total, 1)
    label = "HLG" if transfer == 18 else "PQ" if transfer == 16 else "SDR"
    print(f"self bgra {os.path.basename(path)} [{label}]: frames {total}, within 1 code value of the reference formula {near * 100:.3f} %, largest {worst}")
    return total > 0 and near >= share_limit


def means(path, expected, write):
    shape = info(path, [])
    width, height, cw, ch = shape["width"], shape["height"], shape["chroma_width"], shape["chroma_height"]
    kind = numpy.uint8 if shape["depth"] == 8 else numpy.dtype("<u2")
    rows = []
    for data in frames_of([AMEDIA, "decode", path], shape["frame_bytes"]):
        values = numpy.frombuffer(data, dtype=kind).astype(numpy.float64)
        rows.append((values[:width * height].mean(), values[width * height:width * height + cw * ch].mean(), values[width * height + cw * ch:].mean()))
    if write:
        with open(expected, "w") as out:
            out.write(f"{shape['pix_fmt']}\n" + "".join(f"{y:.3f} {u:.3f} {v:.3f}\n" for y, u, v in rows))
        return True
    lines = open(expected).read().split("\n")
    wanted = [tuple(float(x) for x in line.split()) for line in lines[1:] if line.strip()]
    gap = max((abs(a - b) for got, want in zip(rows, wanted) for a, b in zip(got, want)), default=99.0)
    print(f"means {os.path.basename(path)}: {len(rows)} frames as {shape['pix_fmt']}, largest plane mean difference {gap:.3f} code values")
    return shape["pix_fmt"] == lines[0] and len(rows) == len(wanted) and gap <= 0.5


def fuzz(path, count, seed, extra):
    original = open(path, "rb").read()
    rng = random.Random(seed)
    scratch = os.path.join(HERE, "out", "fuzz_" + os.path.basename(path))
    crashes = 0
    for n in range(count):
        data = bytearray(original)
        if n % 3 == 0:
            data = data[:rng.randrange(1, len(data))]
        else:
            for _ in range(rng.randrange(1, 12)):
                data[rng.randrange(len(data))] = rng.randrange(256)
        open(scratch, "wb").write(data)
        try:
            code = subprocess.run([AMEDIA, "decode", scratch] + extra, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60).returncode
        except subprocess.TimeoutExpired:
            code = -99
        if code not in (0, 1):
            crashes += 1
    os.remove(scratch)
    print(f"fuzz {os.path.basename(path)}{' software' if extra else ''}: {count} damaged copies, {crashes} crashes or hangs")
    return crashes == 0


if __name__ == "__main__":
    kind = sys.argv[1]
    rest = sys.argv[2:]
    extra = ["--software"] if "--software" in rest else []
    rest = [r for r in rest if r != "--software"]
    if kind == "decode":
        ok = decode(rest[0], float(rest[1]) if len(rest) > 1 else 0, extra)
    elif kind == "seek":
        ok = seek(rest[0], int(rest[1]), int(rest[2]), extra)
    elif kind == "sdr":
        ok = sdr(rest[0], float(rest[1]), float(rest[2]))
    elif kind == "hdr":
        ok = hdr(rest[0], float(rest[1]), float(rest[2]))
    elif kind == "selfseek":
        ok = self_seek(rest[0], int(rest[1]), int(rest[2]), extra)
    elif kind == "selfbgra":
        ok = self_bgra(rest[0], float(rest[1]))
    elif kind == "means":
        ok = means(rest[0], rest[1], len(rest) > 2 and rest[2] == "write")
    elif kind == "fuzz":
        ok = fuzz(rest[0], int(rest[1]), int(rest[2]), extra)
    elif kind == "race":
        ok = race(rest[0], rest[1], int(rest[2]))
    elif kind == "near":
        ok = near(rest[0], rest[1], rest[2], int(rest[3]), int(rest[4]), float(rest[5]))
    elif kind == "quality":
        ok = quality(rest[0], rest[1], int(rest[2]), int(rest[3]), float(rest[4]), len(rest) > 5 and rest[5] == "ffmpeg")
    else:
        ok = False
    sys.exit(0 if ok else 1)
