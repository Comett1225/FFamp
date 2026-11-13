import hashlib
import numpy
import os
import subprocess
import sys

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


def both(path):
    size = frame_size(path)
    ours = frames([AMEDIA, "decode", path], size)
    theirs = frames([FFMPEG, "-nostdin", "-v", "quiet", "-i", path, "-map", "0:v:0", "-fps_mode", "passthrough", "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"], size)
    return ours, theirs


def ordered(path, extra):
    ours, theirs = both(path)
    same = sum(a == b for a, b in zip(ours, theirs))
    print(f"{os.path.basename(path)}: frames {len(ours)}, ffmpeg {len(theirs)}, identical in order {same}")
    return len(ours) > 0 and same == len(ours) and 0 <= len(theirs) - len(ours) <= extra


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
        ok = ordered(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 0)
    elif mode == "rangemap":
        ok = rangemap(sys.argv[2], sys.argv[3])
    elif mode == "unordered":
        ok = unordered(sys.argv[2])
    else:
        sys.exit("unknown mode")
    sys.exit(0 if ok else 1)
