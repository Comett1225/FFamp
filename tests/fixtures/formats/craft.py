import struct
import subprocess
import sys


def chunks_of(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "packet=pos,size", "-of", "csv=p=0", path],
                         capture_output=True, text=True, check=True).stdout.split()
    data = open(path, "rb").read()
    found = []
    for line in out:
        size, pos = (int(x) for x in line.split(",")[:2])
        start = pos + 8 if data[pos + 2:pos + 4] == b"dc" else pos
        found.append(data[start:start + size])
    return found


def kind(chunk):
    at = chunk.find(b"\x00\x00\x01\xb6")
    return chunk[at + 4] >> 6 if at >= 0 else -1


def chunk(name, body):
    return name + struct.pack("<I", len(body)) + body + (b"\x00" if len(body) & 1 else b"")


def avi(frames, width, height, rate, handler):
    main = struct.pack("<14I", 1000000 // rate, 0, 0, 0x10, len(frames), 0, 1, 0, width, height, 0, 0, 0, 0)
    stream = b"vids" + handler + struct.pack("<12I", 0, 0, 0, 1, rate, 0, len(frames), 0, 0xFFFFFFFF, 0, 0, (height << 16) | width)
    form = struct.pack("<IiiHH", 40, width, height, 1, 24) + handler + struct.pack("<5I", width * height * 3, 0, 0, 0, 0)
    header = b"hdrl" + chunk(b"avih", main) + chunk(b"LIST", b"strl" + chunk(b"strh", stream) + chunk(b"strf", form))
    movie = b"movi"
    index = b""
    for frame in frames:
        index += b"00dc" + struct.pack("<3I", 0x10 if kind(frame) == 0 else 0, len(movie), len(frame))
        movie += chunk(b"00dc", frame)
    body = b"AVI " + chunk(b"LIST", header) + chunk(b"LIST", movie) + chunk(b"idx1", index)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def packed(source, target):
    frames = [f for f in chunks_of(source) if f]
    first = frames[0].find(b"\x00\x00\x01\xb6")
    frames[0] = frames[0][:first] + b"\x00\x00\x01\xb2DivX503b1393p" + frames[0][first:]
    out = []
    at = 0
    while at < len(frames):
        if at + 1 < len(frames) and kind(frames[at]) != 2 and kind(frames[at + 1]) == 2:
            out.append(frames[at] + frames[at + 1])
            at += 2
            while at < len(frames) and kind(frames[at]) == 2:
                out.append(frames[at])
                at += 1
            out.append(b"\x00\x00\x01\xb6\x50\x00\x00")
        else:
            out.append(frames[at])
            at += 1
    open(target, "wb").write(avi(out, 160, 120, 25, b"XVID"))


def renamed(source, target, names):
    data = open(source, "rb").read()
    for name in names:
        old = name.encode()
        data = data.replace(old, b"JUNK") if old in (b"indx", b"idx1") else data.replace(old, b"JUNQ")
    open(target, "wb").write(data)


def wrapped(source, target, width, height, rate):
    data = open(source, "rb").read()
    frames = []
    at = 0
    while at + 5 <= len(data):
        size = struct.unpack("<I", data[at:at + 4])[0]
        frames.append(data[at + 5:at + 5 + size])
        at += 5 + size
    open(target, "wb").write(avi(frames, width, height, rate, b"XVID"))


if __name__ == "__main__":
    if sys.argv[1] == "packed":
        packed(sys.argv[2], sys.argv[3])
    elif sys.argv[1] == "wrap":
        wrapped(sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]))
    else:
        renamed(sys.argv[2], sys.argv[3], sys.argv[4:])
