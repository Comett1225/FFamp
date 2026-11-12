import struct
import sys

CONTAINERS = {b"moov", b"trak", b"mdia", b"minf", b"stbl", b"edts", b"dinf", b"udta", b"mvex", b"moof", b"traf"}


def walk(data, start, end, depth, limit):
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
            break
        body = data[at + head:min(at + size, at + head + limit)]
        print("  " * depth + kind.decode("latin1"), size, "" if kind in CONTAINERS or kind == b"mdat" else body.hex())
        if kind in CONTAINERS:
            walk(data, at + head, at + size, depth + 1, limit)
        at += size


data = open(sys.argv[1], "rb").read()
walk(data, 0, len(data), 0, int(sys.argv[2]) if len(sys.argv) > 2 else 48)
