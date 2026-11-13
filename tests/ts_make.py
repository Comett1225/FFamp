import os
import random
import struct
import sys


def crc(data):
    value = 0xFFFFFFFF
    for byte in data:
        value ^= byte << 24
        for _ in range(8):
            value = ((value << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if value & 0x80000000 else (value << 1) & 0xFFFFFFFF
    return value


def section(table, body):
    data = bytes([table]) + struct.pack(">H", 0xB000 | (len(body) + 9)) + struct.pack(">H", 1) + bytes([0xC1, 0, 0]) + body
    return data + struct.pack(">I", crc(data))


def packet(pid, counter, payload, starts):
    stuffing = 184 - len(payload)
    head = bytes([0x47, (0x40 if starts else 0) | (pid >> 8), pid & 0xFF])
    if stuffing == 0:
        return head + bytes([0x10 | counter]) + payload
    field = bytes([stuffing - 1]) + (bytes([0]) + b"\xff" * (stuffing - 2) if stuffing > 1 else b"")
    return head + bytes([0x30 | counter]) + field + payload


def table_packet(pid, data):
    return packet(pid, 0, b"\x00" + data + b"\xff" * (183 - len(data)), True)


def stamp(mark, value):
    return bytes([mark << 4 | ((value >> 29) & 0x0E) | 1, (value >> 22) & 0xFF, ((value >> 14) & 0xFE) | 1, (value >> 7) & 0xFF, ((value << 1) & 0xFE) | 1])


def pes(stream, payload, pts=None, dts=None):
    extra = b""
    flags = 0
    if pts is not None and dts is not None and dts != pts:
        extra = stamp(3, pts) + stamp(1, dts)
        flags = 0xC0
    elif pts is not None:
        extra = stamp(2, pts)
        flags = 0x80
    size = len(payload) + 3 + len(extra)
    return b"\x00\x00\x01" + bytes([stream]) + struct.pack(">H", size if size < 65536 and stream != 0xE0 else 0) + bytes([0x80, flags, len(extra)]) + extra + payload


class Mux:
    def __init__(self, streams, sizes=None, seed=1):
        self.out = bytearray()
        self.counters = {}
        self.sizes = sizes
        self.rng = random.Random(seed)
        body = struct.pack(">H", 0xE000 | streams[0][0]) + struct.pack(">H", 0xF000)
        for pid, kind in streams:
            body += bytes([kind]) + struct.pack(">H", 0xE000 | pid) + struct.pack(">H", 0xF000)
        self.out += table_packet(0, section(0, struct.pack(">HH", 1, 0xE000 | 0x1000)))
        self.out += table_packet(0x1000, section(2, body))

    def add(self, pid, data):
        at = 0
        while at < len(data):
            room = self.rng.choice(self.sizes) if self.sizes else 184
            part = data[at:at + room]
            counter = self.counters.get(pid, 0)
            self.counters[pid] = (counter + 1) & 15
            self.out += packet(pid, counter, part, at == 0)
            at += len(part)


def packets_of(data, size=188, lead=0):
    return [data[at + lead:at + lead + 188] for at in range(0, len(data) - size + 1, size)]


def payload_of(p):
    control = (p[3] >> 4) & 3
    start = 4 + (1 + p[4] if control & 2 else 0)
    return p[start:] if control & 1 else b""


def pid_of(p):
    return ((p[1] & 0x1F) << 8) | p[2]


def pes_list(source, pid):
    found = []
    for p in packets_of(source):
        if pid_of(p) != pid:
            continue
        if p[1] & 0x40:
            found.append(bytearray())
        if found:
            found[-1] += payload_of(p)
    return found


def bits(*fields):
    text = ""
    for kind, value in fields:
        if kind == "ue":
            code = bin(value + 1)[2:]
            text += "0" * (len(code) - 1) + code
        else:
            text += bin(value)[2:].zfill(kind)
    text += "1"
    text += "0" * (-len(text) % 8)
    return bytes(int(text[i:i + 8], 2) for i in range(0, len(text), 8))


def fields(target):
    sps = bytes([0x67, 66, 0, 30]) + bits(("ue", 0), ("ue", 0), ("ue", 2), ("ue", 1), (1, 0), ("ue", 19), ("ue", 8), (1, 0), (1, 0), (1, 1), (1, 0), (1, 0))
    pps = bytes([0x68, 0xCE, 0x3C, 0x80])
    code = b"\x00\x00\x00\x01"
    mux = Mux([(0x100, 0x1B)])
    filler = bytes(range(1, 120))
    for frame in range(6):
        for bottom in (0, 1):
            kind = 0x65 if frame == 0 else 0x41
            first = bytes([kind]) + bits(("ue", 0), ("ue", 7), ("ue", 0), (4, frame), (1, 1), (1, bottom)) + filler
            second = bytes([kind]) + bits(("ue", 40), ("ue", 7), ("ue", 0), (4, frame), (1, 1), (1, bottom)) + filler
            unit = code + b"\x09\xf0" + (code + sps + code + pps if frame == 0 and bottom == 0 else b"") + code + first + b"\x00\x00\x01" + second
            mux.add(0x100, pes(0xE0, unit, 90000 + frame * 3600 + bottom * 1800))
    open(target, "wb").write(mux.out)


def remake(source, target, mode):
    data = open(source, "rb").read()
    rng = random.Random(5)
    if mode == "tiny":
        mux = Mux([(0x100, 0x1B), (0x101, 0x0F)], sizes=[1, 2, 3, 5, 7, 11, 184], seed=3)
        video, audio = pes_list(data, 0x100), pes_list(data, 0x101)
        for n in range(max(len(video), len(audio))):
            if n < len(video):
                mux.add(0x100, bytes(video[n]))
            if n < len(audio):
                mux.add(0x101, bytes(audio[n]))
        out = bytes(mux.out)
    elif mode == "wide":
        out = b"".join(p + bytes(16) for p in packets_of(data))
    elif mode == "twice":
        out = b"".join(p + (p if pid_of(p) in (0x100, 0x101) and (p[3] & 0x10) and rng.random() < 0.2 else b"") for p in packets_of(data))
    elif mode == "lost":
        kept, units, lost = [], 0, None
        for p in packets_of(data):
            if pid_of(p) == 0x100 and (p[1] & 0x40):
                units += 1
            if pid_of(p) == 0x100 and units == 12 and lost is None and not (p[1] & 0x40):
                lost = units - 1
                continue
            kept.append(p)
        out = b"".join(kept)
        print(lost)
    elif mode == "spans":
        mux = Mux([(0x100, 0x1B), (0x101, 0x0F)])
        stream = b""
        first = None
        for unit in pes_list(data, 0x101):
            head = 9 + unit[8]
            if first is None:
                first = unit[9:14]
            stream += bytes(unit[head:])
        for unit in pes_list(data, 0x100):
            mux.add(0x100, bytes(unit))
        value = ((first[0] >> 1) & 7) << 30 | first[1] << 22 | (first[2] >> 1) << 15 | first[3] << 7 | first[4] >> 1
        for at in range(0, len(stream), 1000):
            mux.add(0x101, pes(0xC0, stream[at:at + 1000], value if at == 0 else None))
        out = bytes(mux.out)
    open(target, "wb").write(out)


if __name__ == "__main__":
    if sys.argv[1] == "fields":
        fields(sys.argv[2])
    else:
        remake(sys.argv[2], sys.argv[3], sys.argv[1])
