import hashlib
import os
import random
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.environ.get("AMEDIA", os.path.join(HERE, "..", "build", "ffamp.exe" if os.name == "nt" else "ffamp"))
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
FFPROBE = os.environ.get("FFPROBE", "ffprobe")
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


def md5s(data, size):
    return [hashlib.md5(data[i:i + size]).hexdigest() for i in range(0, len(data) // size * size, size)]


def vint(data, at, marker):
    length = 9 - data[at].bit_length()
    value = data[at] if marker else data[at] & ((1 << (8 - length)) - 1)
    for i in range(1, length):
        value = (value << 8) | data[at + i]
    return value, at + length, length


def blocks(data, start, end, found, cluster):
    at = start
    while at < end:
        ident, at, _ = vint(data, at, True)
        size, body, length = vint(data, at, False)
        stop = end if size == (1 << (7 * length)) - 1 else min(body + size, end)
        if ident == 0x1F43B675:
            blocks(data, body, stop, found, [0])
        elif ident in (0x18538067, 0xA0):
            blocks(data, body, stop, found, cluster)
        elif ident == 0xE7:
            cluster[0] = int.from_bytes(data[body:stop], "big")
        elif ident in (0xA3, 0xA1):
            found.append((vint(data, body, False)[1], cluster[0]))
        at = stop


def retimed(wrapped):
    shown = subprocess.run([FFPROBE, "-v", "quiet", "-select_streams", "v:0", "-show_entries", "frame=pts", "-of", "csv=p=0", wrapped],
                           capture_output=True, text=True).stdout.split()
    data = bytearray(open(wrapped, "rb").read())
    found = []
    blocks(data, 0, len(data), found, [0])
    times = [None] * len(found)
    for k, pts in enumerate(int(p.strip(",")) for p in shown if p.strip(",").isdigit()):
        if pts // 40 < len(times):
            times[pts // 40] = k * 40
    for n, (at, cluster) in enumerate(found):
        if times[n] is None:
            times[n] = (times[n - 1] if n else 0) + 20
        data[at:at + 2] = struct.pack(">h", max(-32768, min(32767, times[n] - cluster)))
    path = wrapped[:-4] + "_shown.mkv"
    open(path, "wb").write(data)
    return path


def fate(folder, work, group, through=False):
    rows = [line.strip().split("|") for line in open(os.path.join(HERE, "h264_fate.txt")) if line.strip()]
    passed, failed = 0, []
    for _, name, test in [r for r in rows if r[0] == group]:
        source = os.path.join(folder, *name.split("/"))
        if not os.path.isfile(source):
            failed.append(test)
            print(f"{test}: {name} is missing")
            continue
        wrapped = os.path.join(work, "h264_fate.mkv")
        subprocess.run([FFMPEG, "-nostdin", "-v", "quiet", "-y", "-i", source, "-map", "0:v:0", "-c", "copy", "-bsf:v", "setts=pts=N/(25*TB):dts=N/(25*TB)", wrapped])
        shown = retimed(wrapped) if through else None
        good, results = True, []
        for extra in [None] if through else (["--threads", "1"], [], ["--threads", "4", "--slice"]):
            code, data, shape = ours(wrapped, extra or ["--threads", "1"])
            label = " ".join(extra or []) or "frame threads"
            if through:
                label = subprocess.run([AMEDIA, "videoinfo", shown], capture_output=True, text=True).stdout.partition("backend ")[2].split(" ")[0]
                run = subprocess.run([AMEDIA, "decode", shown], capture_output=True)
                code, data = run.returncode, run.stdout
            if not shape:
                good = False
                results.append(f"{label}: no picture (exit {code})")
                break
            judge = subprocess.run([FFMPEG, "-nostdin", "-v", "quiet", "-threads", "1", "-i", source, "-map", "0:v:0", "-fps_mode", "passthrough",
                                    "-pix_fmt", shape[0], "-f", "framemd5", "-"], capture_output=True).stdout.decode().splitlines()
            theirs_md5 = [line.split(",")[-1].strip() for line in judge if line and not line.startswith("#")]
            mine = md5s(data, frame_size(shape))
            bad = [i for i in range(min(len(mine), len(theirs_md5))) if mine[i] != theirs_md5[i]]
            ok = code == 0 and len(mine) == len(theirs_md5) and mine and not bad
            good &= bool(ok)
            results.append(f"{label} {len(mine)}/{len(theirs_md5)}" + ("" if ok else f" differing {bad[:6]}"))
        print(f"{test} [{shape[0] if shape else '?'}]: {'pass' if good else 'FAIL'}; " + "; ".join(results))
        if good:
            passed += 1
        else:
            failed.append(test)
        os.remove(wrapped)
        if shown:
            os.remove(shown)
    print(f"FATE h264 conformance{' through VideoDecoder' if through else ''}, {group}: {passed} pass, {len(failed)} fail" + (f" ({' '.join(failed)})" if failed else ""))
    return passed > 0 and not failed


def film_grain_sei(period):
    bits = []

    def put(count, value):
        bits.extend((value >> i) & 1 for i in range(count - 1, -1, -1))

    def ue(value):
        value += 1
        put(value.bit_length() - 1, 0)
        put(value.bit_length(), value)

    put(1, 0)
    put(2, 0)
    put(1, 0)
    put(2, 0)
    put(4, 3)
    for _ in range(3):
        put(1, 1)
    models = [[(0, 127, (90, 8, 8)), (128, 255, (60, 10, 6))], [(0, 127, (40, 6, 6)), (128, 255, (30, 9, 9))],
              [(0, 127, (50, 7, 5)), (128, 255, (20, 12, 12))]]
    for intervals in models:
        put(8, len(intervals) - 1)
        put(3, 2)
        for low, high, values in intervals:
            put(8, low)
            put(8, high)
            for v in values:
                ue(2 * v - 1 if v > 0 else -2 * v)
    ue(period)
    while len(bits) % 8:
        bits.append(0)
    payload = bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))
    body, escaped, zeros = bytes([19, len(payload)]) + payload + b"\x80", bytearray(), 0
    for byte in body:
        if zeros >= 2 and byte <= 3:
            escaped.append(3)
            zeros = 0
        escaped.append(byte)
        zeros = zeros + 1 if byte == 0 else 0
    return b"\x00\x00\x00\x01\x06" + bytes(escaped)


def grain(folder):
    raw = subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25,noise=alls=8:allf=t", "-frames:v", "30",
                          "-c:v", "libx264", "-bf", "0", "-x264opts", "aud=1", "-f", "h264", "-"], capture_output=True, check=True).stdout
    marker = b"\x00\x00\x00\x01\x09"
    units = [marker + part for part in raw.split(marker) if part]
    ok = True
    for name, period, every in (("every_picture", 0, True), ("persisting", 1, False)):
        sei = film_grain_sei(period)
        stream = b"".join(unit[:6] + (sei if every or n == 0 else b"") + unit[6:] for n, unit in enumerate(units))
        source, wrapped = os.path.join(folder, "h264_grain.264"), os.path.join(folder, f"h264_grain_{name}.mkv")
        open(source, "wb").write(stream)
        subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-y", "-fflags", "+genpts", "-r", "25", "-i", source, "-c", "copy", wrapped], check=True)
        plain = theirs(wrapped, "yuv420p", ["-threads", "1"])
        ok &= exact(wrapped) and plain != theirs(os.path.join(folder, "h264_grain.264"), "yuv420p", ["-threads", "1", "-export_side_data", "film_grain"])
        os.remove(source)
        os.remove(wrapped)
    return ok


if __name__ == "__main__":
    kind, rest = sys.argv[1], sys.argv[2:]
    if kind == "exact":
        good = all([exact(p) for p in rest])
    elif kind == "damaged":
        good = damaged(rest[0], int(rest[1]), int(rest[2]), rest[3])
    elif kind == "grain":
        good = grain(rest[0])
    elif kind == "fate":
        good = fate(rest[0], rest[1], rest[2], rest[3:] == ["--decode"])
    else:
        good = False
    sys.exit(0 if good else 1)
