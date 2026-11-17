import random
import struct
import sys

CONTAINERS = {b"moov", b"trak", b"edts", b"mdia", b"minf", b"stbl", b"udta"}


def parse(data):
    out = []
    at = 0
    while at + 8 <= len(data):
        size, kind = struct.unpack(">I4s", data[at:at + 8])
        head = 8
        if size == 1:
            size = struct.unpack(">Q", data[at + 8:at + 16])[0]
            head = 16
        elif size == 0:
            size = len(data) - at
        body = data[at + head:at + size]
        out.append([kind, parse(body) if kind in CONTAINERS else body])
        at += size
    return out


def build(boxes):
    out = b""
    for kind, body in boxes:
        raw = build(body) if isinstance(body, list) else body
        out += struct.pack(">I4s", len(raw) + 8, kind) + raw
    return out


def child(boxes, kind):
    return next(body for name, body in boxes if name == kind)


def edits(source, target, entries):
    top = parse(open(source, "rb").read())
    moov = child(top, b"moov")
    header = child(moov, b"mvhd")
    scale = struct.unpack(">I", header[20:24] if header[0] == 1 else header[12:16])[0]
    for trak in [body for name, body in moov if name == b"trak"]:
        if child(child(trak, b"mdia"), b"hdlr")[8:12] != b"vide":
            continue
        body = struct.pack(">II", 0, len(entries))
        for entry in entries:
            seconds, time = entry.split(":")
            body += struct.pack(">IiI", round(float(seconds) * scale), int(time), 0x10000)
        trak[:] = [box for box in trak if box[0] != b"edts"]
        at = next(i for i, box in enumerate(trak) if box[0] == b"tkhd")
        trak.insert(at + 1, [b"edts", [[b"elst", body]]])
    open(target, "wb").write(build(top))


def tagjoin(first, second, target, size):
    noise = random.Random(7)
    picture = b"\x00image/png\x00\x03\x00" + bytes(noise.getrandbits(8) for _ in range(size))
    frame = b"APIC" + struct.pack(">I", len(picture)) + b"\x00\x00" + picture
    length = len(frame)
    safe = bytes([(length >> 21) & 127, (length >> 14) & 127, (length >> 7) & 127, length & 127])
    tag = b"ID3\x03\x00\x00" + safe + frame
    open(target, "wb").write(open(first, "rb").read() + tag + open(second, "rb").read())


def chosen(raw, size, ranges):
    data = open(raw, "rb").read()
    out = sys.stdout.buffer
    for part in ranges.split(","):
        low, high = (int(x) for x in part.split("-"))
        out.write(data[low * size:(high + 1) * size])


def pitch(raw, rate, start, end):
    import numpy
    samples = numpy.frombuffer(open(raw, "rb").read(), "<i2").reshape(-1, 2)[:, 0].astype(float)
    piece = samples[int(float(start) * rate):int(float(end) * rate)]
    spectrum = numpy.abs(numpy.fft.rfft(piece * numpy.hanning(len(piece))))
    print(round(numpy.argmax(spectrum) * rate / len(piece)))


command = sys.argv[1]
if command == "edits":
    edits(sys.argv[2], sys.argv[3], sys.argv[4:])
elif command == "tagjoin":
    tagjoin(sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]))
elif command == "chosen":
    chosen(sys.argv[2], int(sys.argv[3]), sys.argv[4])
elif command == "pitch":
    pitch(sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5])
