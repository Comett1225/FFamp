import os
import re
import subprocess
import sys

import numpy

HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.environ.get("AMEDIA", os.path.join(HERE, "..", "build", "ffamp"))
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")


def info(path):
    words = subprocess.run([AMEDIA, "videoinfo", path], capture_output=True, text=True, timeout=300).stdout.split()
    found = dict(zip(words[0::2], words[1::2]))
    width, height = (int(v) for v in found["size"].split("x"))
    chroma_width, chroma_height = (int(v) for v in found["chroma"].split("x"))
    unit = 1 if int(found["bits"]) == 8 else 2
    return width, height, (width * height + 2 * chroma_width * chroma_height) * unit, width * height * unit


def ours(path, *extra):
    return subprocess.run([AMEDIA, "decode", path, *extra], capture_output=True, timeout=600).stdout


def theirs(path, pix_fmt, *more):
    return subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-i", path, "-map", "0:v:0", "-fps_mode", "passthrough", *more, "-f", "rawvideo", "-pix_fmt", pix_fmt, "-"],
                          capture_output=True, timeout=600).stdout


def planes(path, pix_fmt, alpha):
    width, height, frame, luma = info(path)
    a, b = ours(path), theirs(path, pix_fmt)
    step = frame + (luma if alpha else 0)
    count = len(b) // step if step else 0
    same = sum(a[i * frame:(i + 1) * frame] == b[i * step:i * step + frame] for i in range(count))
    print(f"{os.path.basename(path)}: frames {len(a) // frame if frame else 0}, ffmpeg {count}, identical {same}")
    return count > 0 and len(a) == count * frame and same == count


def luma(path, pix_fmt, slack):
    width, height, frame, plane = info(path)
    a, b = ours(path), theirs(path, pix_fmt)
    step = plane * {"gray": 1, "gray10le": 1, "yuv440p": 2}[pix_fmt]
    count = len(b) // step if step else 0
    kind = numpy.uint8 if plane == width * height else numpy.dtype("<u2")
    worst = 0
    for i in range(count):
        x = numpy.frombuffer(a[i * frame:i * frame + plane], kind).astype(numpy.int32)
        y = numpy.frombuffer(b[i * step:i * step + plane], kind).astype(numpy.int32)
        worst = max(worst, int(numpy.abs(x - y).max()) if len(x) == len(y) else 1 << 16)
    print(f"{os.path.basename(path)}: luma of {count} frames, largest difference {worst}")
    return count > 0 and len(a) == count * frame and worst <= slack


def alpha(path):
    a = numpy.frombuffer(ours(path, "--bgra"), numpy.uint8)
    b = numpy.frombuffer(theirs(path, "bgra"), numpy.uint8)
    print(f"{os.path.basename(path)}: BGRA bytes {len(a)}, ffmpeg {len(b)}")
    return len(a) == len(b) > 0 and bool((a[3::4] == b[3::4]).all())


def rgb(path):
    a, b = ours(path, "--bgra"), theirs(path, "bgra")
    print(f"{os.path.basename(path)}: BGRA bytes {len(a)}, ffmpeg {len(b)}, same {a == b}")
    return len(a) > 0 and a == b


def frame_out(path, layout, out, limit):
    width, height, _, _ = info(path)
    subprocess.run([AMEDIA, "frames-out", path, "--format", layout, "--frame", "0", "--count", "1", "--out", out], check=True, timeout=300)
    a = numpy.fromfile(out, numpy.uint8).astype(numpy.int32)
    more = ["-frames:v", "1"] + (["-vf", "scale=out_color_matrix=bt709:out_range=tv"] if layout == "i420" else [])
    b = numpy.frombuffer(theirs(path, "yuv420p" if layout == "i420" else layout, *more), numpy.uint8).astype(numpy.int32)
    os.remove(out)
    if len(a) != len(b) or not len(a):
        print(f"{os.path.basename(path)}: {layout} sizes {len(a)} and {len(b)}")
        return False
    error = float(numpy.abs(a - b).mean())
    print(f"{os.path.basename(path)}: {layout} frame 0, mean difference {error:.3f}")
    return error <= limit


def taller_field(source, target):
    data = bytearray(open(source, "rb").read())
    heads = [i for i in range(len(data) - 5) if data[i:i + 5] == b"\x00\x00\x02\x80\x01"]
    for at in heads[1::2]:
        data[at + 0x18:at + 0x1A] = (1088).to_bytes(2, "big")
        data[at + 0x16C:at + 0x16E] = (68).to_bytes(2, "big")
        data[at + 0x170 + 34 * 4:at + 0x170 + 68 * 4] = data[at + 0x170:at + 0x170 + 34 * 4]
    open(target, "wb").write(data)
    run = subprocess.run([AMEDIA, "decode", target], capture_output=True, timeout=300)
    print(f"{os.path.basename(target)}: {len(heads)} field headers, second fields made taller, exit {run.returncode}, {len(run.stdout)} bytes out")
    return len(heads) >= 2 and run.returncode == 1 and not run.stdout


def soft(path):
    return subprocess.run([AMEDIA, "softvideo", path], capture_output=True, timeout=600)


def dims(path):
    out = subprocess.run([os.environ.get("FFPROBE", "ffprobe"), "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height", "-of", "csv=p=0", path], capture_output=True, text=True).stdout
    return tuple(int(v) for v in out.split(",")[:2])


def prores_matrix(source, target):
    data = bytearray(open(source, "rb").read())
    heads = [i + 4 for i in range(len(data) - 4) if data[i:i + 4] == b"icpf"]
    for at in heads:
        data[at + 14:at + 17] = bytes([1, 1, 0])
    open(target, "wb").write(data)
    a, b = soft(source), soft(target)
    print(f"{os.path.basename(target)}: {len(heads)} frame headers with primaries 1 and matrix 0, pictures {'the same' if a.stdout == b.stdout else 'different'}")
    return len(heads) > 0 and len(a.stdout) > 0 and a.stdout == b.stdout and re.search(rb"pictures [1-9][0-9]*, 0 of them from RGB", b.stderr) is not None


def soft_chroma(path, limit):
    width, height = dims(path)
    a = numpy.frombuffer(soft(path).stdout, numpy.uint8).astype(numpy.int32)
    b = numpy.frombuffer(theirs(path, "yuv420p"), numpy.uint8).astype(numpy.int32)
    luma, frame = width * height, width * height + 2 * ((width + 1) // 2) * ((height + 1) // 2)
    count = len(b) // frame
    if not count or len(a) != len(b):
        print(f"{os.path.basename(path)}: I420 bytes {len(a)}, ffmpeg {len(b)}")
        return False
    worst = max(float(numpy.abs(a[i * frame + luma:(i + 1) * frame] - b[i * frame + luma:(i + 1) * frame]).mean()) for i in range(count))
    print(f"{os.path.basename(path)}: {count} pictures through SoftVideo, largest mean chroma difference from ffmpeg {worst:.2f}")
    return worst <= limit


def first_mb(nal):
    bits = "".join(f"{byte:08b}" for byte in nal[1:9])
    zeros = bits.index("1")
    return int(bits[zeros:2 * zeros + 1], 2) - 1


def damaged_slice(source, target):
    data = bytearray(open(source, "rb").read())
    sizes = [int(line.split()[3]) for line in subprocess.run([AMEDIA, "packets", source], capture_output=True, text=True).stdout.splitlines() if line[:1].isdigit()]
    at = data.find(b"mdat") + 4
    end, slices = at + sizes[0], []
    while at < end:
        length = int.from_bytes(data[at:at + 4], "big")
        if data[at + 4] & 31 in (1, 5):
            slices.append((at + 4, length))
        at += 4 + length
    start, length = slices[-1]
    mb = first_mb(data[start:start + 9])
    for i in range(start + length // 3, start + length):
        data[i] = (data[i] * 131 + 77) & 255
    open(target, "wb").write(data)
    width, height = dims(source)
    a, b = soft(source).stdout, soft(target).stdout
    if len(slices) < 2 or len(a) != len(b) or not a:
        print(f"{os.path.basename(target)}: slices {len(slices)}, I420 bytes {len(a)} and {len(b)}")
        return False
    row, column = divmod(mb, (width + 15) // 16)
    x = numpy.frombuffer(a[:width * height], numpy.uint8).reshape(height, width)
    y = numpy.frombuffer(b[:width * height], numpy.uint8).reshape(height, width)
    kept = (slice(row * 16, row * 16 + 12), slice(column * 16, min(width, column * 16 + 28)))
    changed = int((x[kept] != y[kept]).sum())
    print(f"{os.path.basename(target)}: last slice from macroblock {mb} damaged from a third of its bytes, {changed} changed samples in the {x[kept].size} of its first two macroblocks")
    return x[kept].size > 0 and changed == 0


def same_decode(first, second):
    a, b = ours(first), ours(second)
    print(f"{os.path.basename(first)} and {os.path.basename(second)}: bytes {len(a)} and {len(b)}")
    return len(a) > 0 and a == b


if __name__ == "__main__":
    kind, rest = sys.argv[1], sys.argv[2:]
    if kind == "planes":
        ok = planes(rest[0], rest[1], len(rest) > 2 and rest[2] == "alpha")
    elif kind == "luma":
        ok = luma(rest[0], rest[1], int(rest[2]) if len(rest) > 2 else 0)
    elif kind == "alpha":
        ok = alpha(rest[0])
    elif kind == "rgb":
        ok = rgb(rest[0])
    elif kind == "out":
        ok = frame_out(rest[0], rest[1], rest[2], float(rest[3]))
    elif kind == "same":
        ok = same_decode(rest[0], rest[1])
    elif kind == "taller":
        ok = taller_field(rest[0], rest[1])
    elif kind == "prores-matrix":
        ok = prores_matrix(rest[0], rest[1])
    elif kind == "soft-chroma":
        ok = soft_chroma(rest[0], float(rest[1]))
    elif kind == "damaged-slice":
        ok = damaged_slice(rest[0], rest[1])
    else:
        ok = False
    sys.exit(0 if ok else 1)
