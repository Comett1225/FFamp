import os
import random
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "wmapro"))
from make import Bits, avi, log2

SRC = os.path.join(HERE, "..", "..", "..", "src")
DECODER = open(os.path.join(SRC, "wmavoice_decode.cpp")).read()
TABLES = open(os.path.join(SRC, "wmavoice_tables.cpp")).read()
START = [int(x) for x in re.search(r"int16_t start\[94\] = \{(.*?)\};", DECODER, re.S).group(1).split(",")]
NAMES = {"AcbNone": 0, "AcbAsymmetric": 1, "AcbHamming": 2, "FcbSilence": 0, "FcbHardcoded": 1, "FcbAw": 2, "FcbPulses": 3}
TYPES = [tuple(NAMES.get(v.strip(), None) if v.strip() in NAMES else int(v) for v in t.split(",")) for t in re.findall(r"\{(\d+, \d+, \w+, \w+, \d+)\}", re.search(r"kTypes\[17\] = \{(.*?)\};", DECODER, re.S).group(1))]
ACB = [float(x) for x in re.findall(r"-?[0-9.]+", re.search(r"kGainAcb\[128\] = \{(.*?)\};", TABLES, re.S).group(1))]
CALM = [i for i, g in enumerate(ACB) if g < 0.9]


def ceil_log2(v):
    return log2(((v - 1) << 1)) if v > 1 else 0


def cdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def clip(a, lo, hi):
    return lo if a < lo else hi if a > hi else a


class Voice:
    def __init__(self, rate, lsps, flags, residual, block, seed):
        self.rate, self.lsps, self.residual, self.block = rate, lsps, residual, block
        self.flags = flags | (0x1000 if lsps == 16 else 0)
        self.rng = random.Random(seed)
        self.low = ((rate << 8) // 400 + 50) >> 8
        self.high = ((rate << 8) * 37 // 2000 + 50) >> 8
        span = self.high - self.low
        self.pitch_bits = ceil_log2(span)
        self.conv = [self.low, (span * 25) >> 6, (span * 44) >> 6, self.high - 1]
        self.hrange = (span >> 3) & ~0xF
        self.delta_bits = 1 + ceil_log2(self.hrange)
        self.bp_range = self.conv[2] + self.conv[3] + 1 + 2 * (self.conv[1] - 2 * self.low)
        self.bp_bits = ceil_log2(self.bp_range)
        self.spill_bits = 3 + ceil_log2(block)
        self.last_pitch, self.last_acb = 40, 0

    def extradata(self):
        tree = Bits()
        for n in range(17):
            tree.put(n // 3, 3)
        tree.bits += [0] * (24 * 8 - len(tree.bits))
        data = bytes(int("".join(map(str, tree.bits[i:i + 8])), 2) for i in range(0, len(tree.bits), 8))
        return bytes(18) + struct.pack("<I", self.flags) + data

    def type_code(self, b, t):
        level, value = (t // 3, t % 3) if t < 18 else (6, t - 18)
        b.put((1 << (2 * level)) - 1, 2 * level)
        b.put(value, 2)

    def lsp_bits(self, b, residual):
        widths = [8, 6, 5, 5] if self.lsps == 10 else [8, 6, 7, 6, 7]
        if residual:
            widths += [5, 7, 6, 6] if self.lsps == 10 else [5, 7, 7, 7]
        for w in widths:
            b.put(self.rng.randrange(1 << w), w)

    def gain(self, b):
        b.put(self.rng.choice(CALM), 7)

    def frame(self, b, t):
        r = self.rng
        blocks, logb, acb, fcb, dbl = TYPES[t]
        self.type_code(b, t)
        pitch = [0] * 8
        cur = 0
        if acb == 1:
            v = r.randrange(1 << self.pitch_bits)
            b.put(v, self.pitch_bits)
            cur = min(self.low + v, self.high - 1)
            last = self.last_pitch
            if self.last_acb == 0 or 20 * abs(cur - last) > cur + last:
                last = cur
            for n in range(blocks):
                fac = 2 * n + 1
                pitch[n] = (fac * cur + (2 * blocks - fac) * last + blocks) >> (logb + 1)
        ext, first_pulses = 0, 0
        if fcb == 0:
            b.put(r.randrange(256), 8)
        elif fcb == 2:
            v = r.randrange(64)
            b.put(v, 6)
            index = v
            if v >= 54:
                ext = 1
                e = r.randrange(4)
                b.put(e, 2)
                index = v + (v - 54) * 3 + e
            offset = START[index]
            while offset < 0:
                offset += pitch[0]
            first_pulses = cdiv(pitch[0] - 1 + 80 - offset, pitch[0])
        last_bp = 0
        for n in range(blocks):
            if acb == 2:
                t1 = (self.conv[1] - self.conv[0]) << 2
                t2 = (self.conv[2] - self.conv[1]) << 1
                t3 = self.conv[3] - self.conv[2] + 1
                if n == 0:
                    v = r.randrange(1 << self.bp_bits)
                    b.put(v, self.bp_bits)
                    bp = v
                else:
                    v = r.randrange(1 << self.delta_bits)
                    b.put(v, self.delta_bits)
                    bp = last_bp - self.hrange + v
                last_bp = clip(bp, self.hrange, self.bp_range - self.hrange)
                if bp < t1:
                    sh2 = (self.conv[0] << 2) + bp
                else:
                    bp -= t1
                    if bp < t2:
                        sh2 = (self.conv[1] << 2) + (bp << 1)
                    else:
                        bp -= t2
                        sh2 = (self.conv[2] + bp) << 2 if bp < t3 else self.conv[3] << 2
                pitch[n] = sh2 >> 2
            if acb == 0:
                if fcb == 1:
                    b.put(r.randrange(256), 8)
                    b.put(r.randrange(64), 6)
                continue
            if fcb == 2:
                b.put(r.randrange(1 << 12), 12 - 2 * (ext and n == 0))
                width = 5 - 2 * n if first_pulses > 0 else 4
                b.put(r.randrange(1 << width), width)
                b.put(r.randrange(2), 1)
            else:
                width = 5 - logb
                for k in range(5):
                    b.put(r.randrange(2), 1)
                    b.put(r.randrange(1 << width), width)
                    if k < dbl:
                        b.put(r.randrange(1 << width), width)
            self.gain(b)
        self.last_acb = acb
        self.last_pitch = 0 if acb == 0 else cur if acb == 1 else pitch[blocks - 1]

    def superframe(self, types, samples=None, stats=False):
        b = Bits()
        b.put(1, 1)
        if samples is None:
            b.put(0, 1)
        else:
            b.put(1, 1)
            b.put(samples, 12)
        if self.residual:
            self.lsp_bits(b, True)
        for t in types:
            if not self.residual:
                self.lsp_bits(b, False)
            self.frame(b, t)
        if stats:
            b.put(1, 1)
            n = self.rng.randrange(4)
            b.put(n, 4)
            for _ in range(n + 1):
                b.put(self.rng.randrange(1024), 10)
        else:
            b.put(0, 1)
        return b.bits

    def packets(self, frames):
        out, queue, spill, sequence = [], list(frames), [], 0
        capacity = self.block * 8
        while queue or spill:
            b = Bits()
            b.put(sequence & 15, 4)
            b.put(1 if self.residual else 0, 1)
            body = list(spill)
            starts = 0
            spill = []
            while queue:
                head = 4 + 1 + 6 * (1 + starts // 63) + self.spill_bits
                left = capacity - head - len(body)
                if left <= 0:
                    break
                f = queue.pop(0)
                starts += 1
                if len(f) <= left:
                    body += f
                else:
                    body += f[:left]
                    spill = f[left:]
                    break
            n = starts
            while n >= 63:
                b.put(63, 6)
                n -= 63
            b.put(n, 6)
            b.put(len(out) and self.carried or 0, self.spill_bits)
            self.carried = len(spill)
            b.bits += body
            b.bits += [0] * (capacity - len(b.bits))
            assert len(b.bits) == capacity
            out.append(bytes(int("".join(map(str, b.bits[i:i + 8])), 2) for i in range(0, capacity, 8)))
            sequence += 1
        return out


class Shape:
    def __init__(self, v, samples):
        self.v, self.block, self.channels, self.samples = v, v.block, 1, samples

    def format(self, packets):
        extra = self.v.extradata()
        rate = sum(len(p) for p in packets) * self.v.rate // self.samples
        return struct.pack("<HHIIHHH", 0x000A, 1, self.v.rate, rate, self.block, 16, len(extra)) + extra


def stream(rate, lsps, flags, residual, block, superframes, seed, group=1, trim=300, music=None):
    v = Voice(rate, lsps, flags, residual, block, seed)
    v.carried = 0
    frames = []
    for i in range(superframes):
        types = [(i * 3 + k) % 17 for k in range(3)] if i < 12 else [v.rng.randrange(17) for _ in range(3)]
        samples = trim if i == superframes - 1 else 401 if i == 7 else None
        frames.append(v.superframe(types, samples, stats=i % 5 == 2))
    if music is not None:
        frames[music][0] = 0
    assert max(len(f) for f in frames) < block * 8 - 64, (max(len(f) for f in frames), block * 8)
    out = v.packets(frames)
    if group > 1:
        out = [b"".join(out[i:i + group]) for i in range(0, len(out), group)]
    v.samples = superframes * 480
    return v, out


def write(target, v, out):
    shape = Shape(v, v.samples)
    if target.endswith(".avi"):
        avi(target, shape, out)
        return
    temporary = target + ".tmp.avi"
    avi(temporary, shape, out)
    ffmpeg = os.environ.get("FFMPEG", "ffmpeg")
    subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-y", "-fflags", "+bitexact", "-i", temporary, "-c", "copy", target], check=True)
    os.remove(temporary)


JOBS = [
    ("voice_8k.wma", dict(rate=8000, lsps=10, flags=0x01 | (9 << 2) | (1 << 7), residual=True, block=150, superframes=50, seed=1, group=2)),
    ("voice_16k.mkv", dict(rate=16000, lsps=16, flags=0x01 | (3 << 2) | 0x40 | (12 << 7) | 0x2000 | 0x4000, residual=False, block=220, superframes=40, seed=2)),
    ("voice_11k.avi", dict(rate=11025, lsps=10, flags=0x2000 | 0x4000, residual=True, block=140, superframes=36, seed=3, group=3)),
    ("voice_music.avi", dict(rate=8000, lsps=10, flags=0x01, residual=False, block=150, superframes=10, seed=5, music=6)),
    ("voice_22k.wma", dict(rate=22050, lsps=16, flags=0x01 | (11 << 2) | 0x40 | (3 << 7), residual=True, block=200, superframes=40, seed=4)),
]


def main(args):
    if len(args) == 3 and args[0] == "long":
        v, out = stream(16000, 16, 0x01 | (5 << 2) | 0x40, True, 200, int(float(args[1]) * 16000 / 480), 9)
        write(args[2], v, out)
        return
    folder = args[0] if args else HERE
    for name, job in JOBS:
        v, out = stream(**job)
        write(os.path.join(folder, name), v, out)
        print(name, len(out), "packets")


if __name__ == "__main__":
    main(sys.argv[1:])
