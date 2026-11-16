import os
import random
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.environ.get("AMEDIA", os.path.join(HERE, "..", "build", "ffamp.exe" if os.name == "nt" else "ffamp"))
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
LAYOUTS = {"420": (1, 1), "422": (1, 0), "444": (0, 0)}


def ours(path, extra, env=None):
    run = subprocess.run([AMEDIA, "h264", path] + extra, capture_output=True, env=env)
    shape = None
    for line in run.stderr.decode(errors="replace").splitlines():
        if line.startswith("format "):
            words = line.split()
            width, height = (int(v) for v in words[3].split("x"))
            shape = (words[1], width, height)
    return run.returncode, run.stdout, shape


def theirs(path, pix_fmt, more=()):
    return subprocess.run([FFMPEG, "-nostdin", "-v", "quiet", *more, "-i", path, "-map", "0:v:0", "-fps_mode", "passthrough",
                           "-pix_fmt", pix_fmt, "-f", "rawvideo", "-"], capture_output=True).stdout


def frame_size(shape):
    pix_fmt, width, height = shape
    layout = "444" if pix_fmt.startswith("gbr") else pix_fmt[3:6]
    sx, sy = LAYOUTS[layout]
    chroma = (-(-width >> sx)) * (-(-height >> sy))
    return (width * height + 2 * chroma) * (2 if pix_fmt.endswith("le") else 1)


def compare(a, b, size):
    frames = min(len(a), len(b)) // size
    bad = [i for i in range(frames) if a[i * size:(i + 1) * size] != b[i * size:(i + 1) * size]]
    return len(a) // size, len(b) // size, bad


def exact(path):
    ok = True
    for extra in (["--threads", "1"], [], ["--threads", "4", "--slice"]):
        code, data, shape = ours(path, extra)
        if not shape:
            print(f"{os.path.basename(path)} {' '.join(extra) or 'frame threads'}: no picture (exit {code})")
            return False
        reference = theirs(path, shape[0], ["-threads", "1"] if extra[:2] == ["--threads", "1"] else [])
        mine, judge, bad = compare(data, reference, frame_size(shape))
        good = code == 0 and mine == judge and mine > 0 and not bad
        ok &= good
        print(f"{os.path.basename(path)} [{shape[0]} {shape[1]}x{shape[2]}] {' '.join(extra) or 'frame threads'}: frames {mine}/{judge}, differing {len(bad)} {bad[:6]}")
    return ok


def boxes(data, start, end):
    at = start
    while at + 8 <= end:
        size, kind = struct.unpack(">I4s", data[at:at + 8])
        head = 8
        if size == 1:
            size = struct.unpack(">Q", data[at + 8:at + 16])[0]
            head = 16
        elif size == 0:
            size = end - at
        if size < head:
            return
        yield kind, at + head, at + size
        at += size


def damage(source, target, seed):
    data = bytearray(open(source, "rb").read())
    begin, end = [(b, e) for kind, b, e in boxes(data, 0, len(data)) if kind == b"mdat"][0]
    begin += (end - begin) // 10
    rng = random.Random(seed)
    for _ in range(20):
        at = rng.randrange(begin, end)
        data[at] ^= 1 << rng.randrange(8)
    for _ in range(4):
        at = rng.randrange(begin, end - 600)
        count = rng.randrange(50, 600)
        data[at:at + count] = bytes(count)
    open(target, "wb").write(data)


def damaged(source, count, seed, folder):
    exact_count = undefined = 0
    ok = True
    for n in range(count):
        path = os.path.join(folder, f"h264_damaged_{n}.mp4")
        damage(source, path, seed * 1000 + n)
        code, data, shape = ours(path, ["--threads", "1"])
        threaded, _, _ = ours(path, [])
        if code != 0 or threaded != 0 or not shape:
            print(f"damaged copy {n}: exit {code}/{threaded}")
            ok = False
            continue
        size = frame_size(shape)
        plain = theirs(path, shape[0], ["-threads", "1"])
        mine, judge, bad = compare(data, plain, size)
        if mine != judge:
            print(f"damaged copy {n}: {mine} frames against FFmpeg's {judge}")
            ok = False
        elif not bad:
            exact_count += 1
        else:
            filtered = subprocess.run([FFMPEG, "-nostdin", "-v", "quiet", "-threads", "1", "-i", path, "-map", "0:v:0", "-fps_mode", "passthrough",
                                       "-vf", "null,format=" + shape[0], "-f", "rawvideo", "-"], capture_output=True).stdout
            filled = ours(path, ["--threads", "1"], dict(os.environ, FFAMP_H264_FILL="85"))[1]
            if filtered != plain or filled != data:
                undefined += 1
            else:
                print(f"damaged copy {n}: frames {bad[:6]} differ where FFmpeg's own output is stable")
                ok = False
        os.remove(path)
    print(f"damaged copies of {os.path.basename(source)}: {count}, bit-exact {exact_count}, differing only where FFmpeg reads unwritten frame memory {undefined}")
    return ok


def conformance(folder, work):
    names = sorted(n for n in os.listdir(folder) if os.path.splitext(n)[1].lower() in (".264", ".h264", ".jsv", ".jvt", ".26l", ".avc", ".bit", ".bits"))
    passed = failed = 0
    for name in names:
        wrapped = os.path.join(work, "conformance.mkv")
        subprocess.run([FFMPEG, "-nostdin", "-v", "quiet", "-y", "-i", os.path.join(folder, name), "-map", "0:v:0", "-c", "copy", wrapped])
        if exact(wrapped):
            passed += 1
        else:
            failed += 1
            print(f"conformance {name}: FAIL")
    print(f"conformance: {passed} pass, {failed} fail of {len(names)}")
    return failed == 0 and passed > 0


if __name__ == "__main__":
    kind, rest = sys.argv[1], sys.argv[2:]
    if kind == "exact":
        good = all([exact(p) for p in rest])
    elif kind == "damaged":
        good = damaged(rest[0], int(rest[1]), int(rest[2]), rest[3])
    elif kind == "conformance":
        good = conformance(rest[0], rest[1])
    else:
        good = False
    sys.exit(0 if good else 1)
