import hashlib
import numpy
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.environ.get("AMEDIA", os.path.join(HERE, "..", "build", "ffamp"))
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")


def frame_size(path):
    words = subprocess.run([AMEDIA, "videoinfo", path], capture_output=True, text=True, check=True).stdout.split()
    found = dict(zip(words[0::2], words[1::2]))
    width, height = (int(v) for v in found["size"].split("x"))
    return width * height + 2 * ((width + 1) // 2) * ((height + 1) // 2)


def frames(command, size):
    data = subprocess.run(command, capture_output=True).stdout
    return [hashlib.md5(data[i:i + size]).hexdigest() for i in range(0, len(data) - size + 1, size)]


def both(path, flags=()):
    size = frame_size(path)
    ours = frames([AMEDIA, "decode", path], size)
    theirs = frames([FFMPEG, "-nostdin", "-v", "quiet", *flags, "-i", path, "-map", "0:v:0", "-fps_mode", "passthrough", "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"], size)
    return ours, theirs


def ordered(path, extra, flags=()):
    ours, theirs = both(path, flags)
    same = sum(a == b for a, b in zip(ours, theirs))
    print(f"{os.path.basename(path)}: frames {len(ours)}, ffmpeg {len(theirs)}, identical in order {same}")
    return len(ours) > 0 and same == len(ours) and 0 <= len(theirs) - len(ours) <= extra


def prefix(path, count):
    ours, theirs = both(path)
    same = sum(a == b for a, b in zip(ours, theirs))
    print(f"{os.path.basename(path)}: frames {len(ours)}, ffmpeg {len(theirs)} (its demuxer stops early), the first {same} identical")
    return len(theirs) == count and same == count and len(ours) > count


def best(command):
    fastest = None
    for _ in range(3):
        start = time.perf_counter()
        if subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0:
            return None
        spent = time.perf_counter() - start
        fastest = spent if fastest is None else min(fastest, spent)
    return fastest


def speed(path, limit):
    mine = best([AMEDIA, "decode", path])
    theirs = best([FFMPEG, "-nostdin", "-v", "error", "-threads", "1", "-i", path, "-map", "0:v:0", "-fps_mode", "passthrough", "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"])
    if mine is None or theirs is None:
        print(f"{os.path.basename(path)}: a decode failed")
        return False
    print(f"{os.path.basename(path)}: ours {mine:.3f} s, ffmpeg on one thread {theirs:.3f} s, ratio {mine / theirs:.2f} (limit {limit})")
    return mine <= theirs * limit


def seed(path, out, count):
    words = subprocess.run([AMEDIA, "videoinfo", path], capture_output=True, text=True, check=True).stdout.split()
    found = dict(zip(words[0::2], words[1::2]))
    width, height = (int(v) for v in found["size"].split("x"))
    lines = subprocess.run([AMEDIA, "packets", path], capture_output=True, text=True).stdout.splitlines()
    number = next(l.split()[1] for l in lines if l.startswith("track") and " video " in l)
    sizes = [int(l.split()[3]) for l in lines if l[:1].isdigit() or l[:1] == "-"][:count]
    raw = subprocess.run([AMEDIA, "packets", path, "--raw", "--track", number], capture_output=True).stdout
    versions = ["MPG4", "MP42", "MP43", "WMV1", "WMV2"]
    head = open(path, "rb").read()
    at = head.find(b"strf")
    extra = head[at + 48:at + 52] if found["codec"] == "WMV2" and at >= 0 else bytes(4)
    data = bytearray([versions.index(found["codec"])]) + width.to_bytes(2, "big") + height.to_bytes(2, "big") + extra
    at = 0
    for n in sizes:
        data += n.to_bytes(2, "big") + raw[at:at + n]
        at += n
    open(out, "wb").write(data)
    return len(sizes) > 0


def unordered(path):
    ours, theirs = both(path)
    same = len(set(ours) & set(theirs))
    print(f"{os.path.basename(path)}: frames {len(ours)}, ffmpeg {len(theirs)}, every picture found {sorted(ours) == sorted(theirs)} ({same} distinct in common)")
    return len(ours) > 0 and sorted(ours) == sorted(theirs)


def rangemap(path, scratch):
    subprocess.run([FFMPEG, "-nostdin", "-v", "quiet", "-y", "-i", path, "-an", "-c", "copy", scratch], check=True)
    data = bytearray(open(scratch, "rb").read())
    entry = bytes([0, 0, 1, 0x0E, 0x10, 0x44, 0x59, 0xC3, 0xBC, 0x80])
    at = data.find(entry)
    if at < 0 or data.find(entry, at + 1) >= 0:
        print("entry point not found once")
        return False
    data[at + 8] = 0xBE
    data[at + 9] = 0xB6
    open(scratch, "wb").write(data)

    size = frame_size(scratch)
    raw = subprocess.run([AMEDIA, "decode", scratch], capture_output=True).stdout
    reference = subprocess.run([FFMPEG, "-nostdin", "-v", "quiet", "-i", scratch, "-map", "0:v:0", "-fps_mode", "passthrough", "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"], capture_output=True).stdout
    os.remove(scratch)
    count = len(reference) // size
    luma = size * 2 // 3
    expected = numpy.frombuffer(reference, numpy.uint8).astype(numpy.int32).reshape(count, size)
    factors = numpy.full(size, 14, numpy.int32)
    factors[:luma] = 11
    expected = numpy.clip((((expected - 128) * factors + 4) >> 3) + 128, 0, 255).astype(numpy.uint8)
    ours = numpy.frombuffer(raw, numpy.uint8)
    same = len(raw) == len(reference) and bool((ours.reshape(count, size) == expected).all())
    print(f"{os.path.basename(path)} with RANGE_MAPY 2 and RANGE_MAPUV 5: frames {len(raw) // size}, ffmpeg {count}, every frame mapped as ST 421 6.2.15 and 6.2.16 say {same}")
    return same


if __name__ == "__main__":
    mode = sys.argv[1]
    if mode == "ordered":
        ok = ordered(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 0, sys.argv[4:])
    elif mode == "prefix":
        ok = prefix(sys.argv[2], int(sys.argv[3]))
    elif mode == "speed":
        ok = speed(sys.argv[2], float(sys.argv[3]))
    elif mode == "seed":
        ok = seed(sys.argv[2], sys.argv[3], int(sys.argv[4]))
    elif mode == "rangemap":
        ok = rangemap(sys.argv[2], sys.argv[3])
    elif mode == "unordered":
        ok = unordered(sys.argv[2])
    else:
        sys.exit("unknown mode")
    sys.exit(0 if ok else 1)
