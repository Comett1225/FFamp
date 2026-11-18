import os
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
    step = int(plane * {"gray": 1, "gray10le": 1, "yuv440p": 2, "yuv410p": 1.125, "yuv411p": 1.5}[pix_fmt])
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
    else:
        ok = False
    sys.exit(0 if ok else 1)
