import math
import os
import random
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "wmapro"))
from make import Bits, avi, log2


def ceil_log2(v):
    return log2(((v - 1) << 1) & 0xFFFFFFFF) if v > 0 else 0


class Lossless:
    def __init__(self, rate, channels, mask, bits, flags, seed):
        self.rate, self.channels, self.mask, self.depth, self.flags = rate, channels, mask, bits, flags
        frame = 9 if rate <= 16000 else 10 if rate <= 22050 else 11 if rate <= 48000 else 12 if rate <= 96000 else 13
        frame += {0: 0, 2: 1, 4: -1, 6: -2}[flags & 6]
        self.spf = 1 << frame
        self.most = 1 << ((flags & 0x38) >> 3)
        self.least = self.spf // self.most
        self.prefix = bool(flags & 0x40)
        self.rng = random.Random(seed)
        self.average = [0] * channels
        self.moving = 0
        self.phase = 0.0

    def residue(self, b, c, value):
        u = (value << 1) ^ (value >> 63) if value >= 0 else ((-value) << 1) - 1
        mean = (self.average[c] + (1 << self.moving)) >> (self.moving + 1)
        width = 0 if mean <= 1 else ceil_log2(mean)
        quo, rem = u >> width, u & ((1 << width) - 1)
        if quo >= 32:
            b.put((1 << 32) - 1, 32)
            b.put(0, 1)
            extra = quo - 32
            n = max(1, extra.bit_length())
            b.put(n - 1, 5)
            b.put(extra, n)
        else:
            b.put((1 << quo) - 1, quo)
            b.put(0, 1)
        if width:
            b.put(rem, width)
        self.average[c] = (u + self.average[c] - (self.average[c] >> self.moving)) & 0xFFFFFFFF

    def signal(self, n, scale):
        out = []
        for _ in range(n):
            self.phase += 0.07
            out.append(int(scale * math.sin(self.phase)) + self.rng.randint(-3, 3))
        return out

    def subframe(self, b, length, plan, first):
        seekable = first and plan.get("seekable", True)
        b.put(1 if seekable else 0, 1)
        if seekable:
            b.put(0, 1)
            b.put(1 if plan.get("ac") else 0, 1)
            b.put(1 if plan.get("inter") else 0, 1)
            b.put(1 if plan.get("mclms") else 0, 1)
            if plan.get("ac"):
                b.put(2, 4)
                b.put(3, 4)
                for k in range(3):
                    b.put(k + 2, 3)
            if plan.get("mclms"):
                order = 4
                b.put(order // 2 - 1, 4)
                b.put(5, 4)
                b.put(1, 1)
                send = 4
                b.put(send - 2, ceil_log2(6))
                for _ in range(order * self.channels * self.channels + self.channels * (self.channels - 1) // 2):
                    b.put(self.rng.randrange(1 << send), send)
            b.put(1, 1)
            for c in range(self.channels):
                orders = plan.get("orders", [32, 16])
                b.put(len(orders) - 1, 3)
                for o in orders:
                    b.put(o // 8 - 1, 7)
                for _ in orders:
                    b.put(10, 4)
                for o in orders:
                    b.put(o - 1, ceil_log2(o))
                    b.put(3, ceil_log2(11))
                    for _ in range(o):
                        b.put(self.rng.randrange(32), 5)
            self.moving = 3
            b.put(self.moving, 3)
            b.put(plan.get("quant", 1) - 1, 8)
        raw = plan.get("raw", False)
        b.put(1 if raw else 0, 1)
        coded = [c != plan.get("silent") for c in range(self.channels)]
        if not raw:
            for c in range(self.channels):
                b.put(1 if coded[c] else 0, 1)
            if self.flags & 0x100:
                b.put(0, 1)
        padding = plan.get("padding", 0)
        if padding:
            b.put(1, 1)
            b.put(padding, 5)
        else:
            b.put(0, 1)
        if raw:
            width = self.depth - padding
            for c in range(self.channels):
                for v in self.signal(length, (1 << (width - 2))):
                    b.put(v & ((1 << width) - 1), width)
            return
        for c in range(self.channels):
            if not coded[c]:
                continue
            b.put(0, 1)
            values = [v // 40 for v in self.signal(length, 1 << (self.depth - 4))]
            values[self.rng.randrange(length)] = self.rng.choice((-1, 1)) * (1 << (self.depth - 3))
            at = 0
            if seekable:
                mean = 5
                b.put(mean, self.depth)
                self.average[c] = mean << (self.moving + 1)
                width = self.depth + (1 if plan.get("inter") else 0)
                b.put(values[0] & ((1 << width) - 1), width)
                at = 1
            for v in values[at:]:
                self.residue(b, c, v)

    def frame(self, plan, index, total):
        b = Bits()
        b.put(1, 1)
        layout = plan.get("layout", [self.spf])
        offset = 0
        for length in layout:
            if offset != self.spf - self.least:
                b.put(length // self.least - 1, log2(self.most - 1) + 1 if self.most > 1 else 1)
            offset += length
        if self.flags & 0x80:
            b.put(self.rng.randrange(256), 8)
        start = plan.get("start") if index == 0 else 0
        end = plan.get("end") if index == total - 1 else 0
        if start or end:
            b.put(1, 1)
            for value in (start, end):
                b.put(1 if value else 0, 1)
                if value:
                    b.put(value, log2(self.spf * 2))
        else:
            b.put(0, 1)
        for i, length in enumerate(layout):
            self.subframe(b, length, plan, i == 0 and index % plan.get("every", 3) == 0)
        if self.prefix:
            head = Bits()
            head.put(self.size_bits + len(b.bits) + 2, self.size_bits)
            return head.bits + b.bits + [0, 1]
        return b.bits + [1]


def make(name, rate, channels, mask, bits, flags, frames, plan, seed):
    def build(block):
        s = Lossless(rate, channels, mask, bits, flags, seed)
        s.block, s.size_bits = block, log2(block) + 4
        return s, [s.frame(plan, i, frames) for i in range(frames)]

    _, made = build(4096)
    s, made = build(max(len(f) for f in made) // 8 * 5 // 4 + 16)
    s.depth_field = bits
    return s, made


def packets(s, frames):
    out, sequence, pending, queue = [], 0, [], list(frames)
    while queue or pending:
        b = Bits()
        b.put(sequence & 15, 4)
        b.put(0, 2)
        b.put(len(pending), s.size_bits)
        b.bits += pending
        pending = []
        while queue:
            left = s.block * 8 - len(b.bits)
            if not left:
                break
            if len(queue[0]) <= left:
                b.bits += queue.pop(0)
            else:
                b.bits += queue[0][:left]
                pending = queue.pop(0)[left:]
                break
        b.bits += [0] * (s.block * 8 - len(b.bits))
        out.append(bytes(int("".join(map(str, b.bits[i:i + 8])), 2) for i in range(0, len(b.bits), 8)))
        sequence += 1
    return out


class Shape:
    def __init__(self, s, count, frames):
        self.s, self.block, self.channels, self.frames, self.spf = s, s.block, s.channels, frames, s.spf

    def format(self, packets):
        extra = struct.pack("<HI8xHH", self.s.depth, self.s.mask, self.s.flags, 0)
        rate = self.block * len(packets) * self.s.rate // (self.spf * self.frames)
        return struct.pack("<HHIIHHH", 0x163, self.channels, self.s.rate, rate, self.block, self.s.depth, len(extra)) + extra


def main(folder):
    ffmpeg = os.environ.get("FFMPEG", "ffmpeg")
    jobs = [
        ("lossless_stereo16", ".wma", dict(rate=44100, channels=2, mask=0x3, bits=16, flags=0x40, frames=10, plan={"ac": True, "inter": True, "start": 300, "end": 500})),
        ("lossless_stereo24", ".wma", dict(rate=48000, channels=2, mask=0x3, bits=24, flags=0x50, frames=7, plan={"inter": True, "mclms": True, "layout": [1024, 512, 512], "orders": [32, 16, 8]})),
        ("lossless_mono", ".mkv", dict(rate=22050, channels=1, mask=0x4, bits=16, flags=0x00, frames=12, plan={"orders": [16], "quant": 3, "every": 4})),
        ("lossless_raw", ".avi", dict(rate=32000, channels=2, mask=0x3, bits=16, flags=0xC0, frames=6, plan={"raw": True, "padding": 2})),
        ("lossless_51", ".wma", dict(rate=48000, channels=6, mask=0x3F, bits=16, flags=0x140, frames=5, plan={"mclms": True, "silent": 3, "orders": [16]})),
    ]
    for name, kind, job in jobs:
        frames = job["frames"]
        s, made = make(name, seed=len(name), **job)
        shape = Shape(s, 0, frames)
        out = packets(s, made)
        target = os.path.join(folder, name + kind)
        temporary = target + (".tmp.avi" if kind != ".avi" else "")
        avi(temporary if kind != ".avi" else target, shape, out)
        if kind != ".avi":
            subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-y", "-fflags", "+bitexact", "-i", temporary, "-c", "copy", target], check=True)
            os.remove(temporary)
        print(name + kind, len(out), "packets")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else HERE)
