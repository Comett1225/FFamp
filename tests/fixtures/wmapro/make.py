import os
import random
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "..", "..", "src", "wmapro_decode.cpp")


def load():
    found = {}
    for m in re.finditer(r"const (\w+) (k\w+)\[\d+\] = \{(.*?)\};", open(SOURCE).read(), re.S):
        cast = float if m.group(1) == "float" else int
        found[m.group(2)] = [cast(x) for x in re.findall(r"-?[0-9.]+", m.group(3))]
    return found


T = load()


def codes(lengths, symbols, offset=0):
    table, code = {}, 0
    for length, symbol in zip(lengths, symbols):
        table.setdefault(symbol + offset, (code >> (32 - length), length))
        code += 1 << (32 - length)
    return table


VEC4 = codes(T["kVec4Lengths"], T["kVec4Symbols"], -1)
VEC2 = codes(T["kVec2Lengths"], T["kVec2Symbols"], -1)
VEC1 = codes(T["kVec1Lengths"], T["kVec1Symbols"])
SCALE = codes(T["kScaleLengths"], T["kScaleSymbols"], -60)
SCALE_RUN = codes(T["kScaleRunLengths"], T["kScaleRunSymbols"])
COEF0 = codes(T["kCoef0Lengths"], T["kCoef0Symbols"])
RUN_LEVEL = {}
for s in range(2, len(T["kCoef0Runs"])):
    RUN_LEVEL.setdefault((T["kCoef0Runs"][s], int(T["kCoef0Levels"][s])), s)


def log2(v):
    return v.bit_length() - 1


class Bits:
    def __init__(self):
        self.bits = []

    def put(self, value, count):
        for i in reversed(range(count)):
            self.bits.append((value >> i) & 1)

    def code(self, pair):
        self.put(pair[0], pair[1])

    def large(self, value):
        if value < 1 << 8:
            self.put(0, 1)
            self.put(value, 8)
        elif value < 1 << 16:
            self.put(2, 2)
            self.put(value, 16)
        elif value < 1 << 24:
            self.put(6, 3)
            self.put(value, 24)
        else:
            self.put(7, 3)
            self.put(value, 31)


class Stream:
    def __init__(self, rate, channels, mask, bits, flags, block, seed):
        self.rate, self.channels, self.mask, self.depth, self.flags, self.block = rate, channels, mask, bits, flags, block
        frame = 9 if rate <= 16000 else 10 if rate <= 22050 else 11 if rate <= 48000 else 12 if rate <= 96000 else 13
        frame += {0: 0, 2: 1, 4: -1, 6: -2}[flags & 6]
        self.spf = 1 << frame
        sub = (flags & 0x38) >> 3
        self.most = 1 << sub
        self.marker = self.most in (4, 16)
        self.length_bits = log2(sub) + 1 if sub else 1
        self.least = self.spf // self.most
        self.size_bits = log2(block) + 4
        self.rng = random.Random(seed)

    def bands(self, length):
        offsets = [0]
        for x in range(28):
            if offsets[-1] >= length:
                break
            at = ((length * 2 * T["kBandFrequencies"][x]) // self.rate + 2) & ~3
            if at > offsets[-1]:
                offsets.append(at)
            if at >= length:
                break
        offsets[-1] = length
        return len(offsets) - 1

    def length_code(self, b, length):
        shift = log2(self.spf // length)
        if self.marker:
            b.put(1 if shift else 0, 1)
            if shift:
                b.put(shift - 1, self.length_bits - 1)
        else:
            b.put(shift, self.length_bits)

    def tiles(self, b, layouts):
        fixed = all(l == layouts[0] for l in layouts)
        if self.most > 1:
            b.put(1 if fixed else 0, 1)
        samples = [0] * self.channels
        taken = [0] * self.channels
        current = self.channels
        least = 0
        while least < self.spf:
            first = next(c for c in range(self.channels) if samples[c] == least)
            length = layouts[first][taken[first]]
            contains = []
            for c in range(self.channels):
                if samples[c] != least:
                    contains.append(False)
                elif fixed or current == 1 or least == self.spf - self.least:
                    contains.append(True)
                else:
                    contains.append(layouts[c][taken[c]] == length)
                    b.put(1 if contains[c] else 0, 1)
            if least != self.spf - self.least:
                self.length_code(b, length)
            least += length
            for c in range(self.channels):
                if contains[c]:
                    taken[c] += 1
                    samples[c] += length
                elif samples[c] <= least:
                    if samples[c] < least:
                        current = 0
                        least = samples[c]
                    current += 1

    def spectrum(self, c, length):
        v = [0] * length
        top = max(8, length // 6)
        for i in range(top):
            r = self.rng.random()
            v[i] = self.rng.choice((-1, 1)) * (0 if r < 0.4 else 1 if r < 0.7 else self.rng.randint(2, 15) if r < 0.95 else self.rng.randint(16, 140))
        for k in range(1 + c % 3):
            at = self.rng.randrange(1, top)
            v[at] = self.rng.choice((-1, 1)) * self.rng.choice((300, 2000, 70000))
        if length >= 256:
            v[length - 3] = 5
        return v

    def coefficients(self, b, values, length, vectors, explicit):
        b.put(0, 1)
        at, zeros, rl = 0, 0, False
        while (explicit or not rl) and at + 3 < vectors:
            mags = [abs(x) for x in values[at:at + 4]]
            key = mags[0] << 12 | mags[1] << 8 | mags[2] << 4 | mags[3]
            if max(mags) < 16 and key in VEC4:
                b.code(VEC4[key])
            else:
                b.code(VEC4[-1])
                for m, n in ((mags[0], mags[1]), (mags[2], mags[3])):
                    if max(m, n) < 16 and (m << 4 | n) in VEC2:
                        b.code(VEC2[m << 4 | n])
                    else:
                        b.code(VEC2[-1])
                        for x in (m, n):
                            if x < 100:
                                b.code(VEC1[x])
                            else:
                                b.code(VEC1[100])
                                b.large(x - 100)
            for x in values[at:at + 4]:
                if x:
                    b.put(1 if x > 0 else 0, 1)
                    zeros = 0
                else:
                    zeros += 1
                    rl = rl or zeros > length >> 8
            at += 4
        if at >= length:
            return
        escape = log2(length - 1) + 1
        offset = at
        for p in range(at, length):
            if not values[p]:
                continue
            run, level = p - offset, abs(values[p])
            if (run, level) in RUN_LEVEL:
                b.code(COEF0[RUN_LEVEL[(run, level)]])
            else:
                b.code(COEF0[0])
                b.large(level)
                if run == 0:
                    b.put(0, 1)
                elif run <= 4:
                    b.put(2, 2)
                    b.put(run - 1, 2)
                else:
                    b.put(6, 3)
                    b.put(run - 4, escape)
            b.put(1 if values[p] > 0 else 0, 1)
            offset = p + 1
        if offset < length:
            b.code(COEF0[1])

    def subframe(self, b, sub, length, current, reuse, plan):
        if plan.get("fill"):
            b.put(1, 1)
            b.put(plan["fill"], 2)
            b.put(0, plan["fill"])
        else:
            b.put(0, 1)
        b.put(0, 1)
        bands = self.bands(length)
        if self.channels > 1:
            b.put(0, 1)
            left = list(sub)
            for group, kind in plan["groups"](sub):
                if not left:
                    break
                if len(left) > 2:
                    for c in sub:
                        if c in left:
                            b.put(1 if c in group else 0, 1)
                else:
                    group = left
                n = len(group)
                left = [c for c in left if c not in group]
                transform = kind != "none"
                if n == 2:
                    b.put(0, 1) if transform else b.put(2, 2)
                elif n > 2:
                    b.put(1 if transform else 0, 1)
                    if transform:
                        b.put(1 if kind == "rotate" else 0, 1)
                        if kind == "rotate":
                            for _ in range(n * (n - 1) // 2):
                                b.put(self.rng.randrange(64), 6)
                            for _ in range(n):
                                b.put(self.rng.randrange(2), 1)
                else:
                    transform = False
                if transform:
                    if plan.get("per_band"):
                        b.put(0, 1)
                        for i in range(bands):
                            b.put(i % 3 != 1, 1)
                    else:
                        b.put(1, 1)
        sending = [c for c in sub if c != plan.get("silent")]
        for c in sub:
            b.put(1 if c in sending else 0, 1)
        if not sending:
            return
        explicit = plan.get("vectors")
        b.put(1 if explicit else 0, 1)
        counts = {}
        for c in sub:
            counts[c] = length if not explicit else min(length, ((length // 3) // 4) * 4)
            if explicit:
                b.put(counts[c] // 4, log2((length + 3) // 4) + 1)
        step = plan.get("step", 0)
        b.put(step & 63, 6)
        if step in (31, -32):
            b.put(31, 5)
            b.put(3, 5)
        if len(sub) > 1:
            b.put(2, 3)
            for i, c in enumerate(sub):
                b.put(i & 1, 1)
                if i & 1:
                    b.put(1, 2)
        for c in sub:
            if current[c] and not plan.get("resend"):
                b.put(0, 1)
            else:
                if current[c]:
                    b.put(1, 1)
                if reuse[c]:
                    b.code(SCALE_RUN[1])
                else:
                    b.put(1, 2)
                    for i in range(bands):
                        b.code(SCALE[(i % 5) - 2 if i else 0])
                reuse[c] = True
        for c in sub:
            if c in sending:
                self.coefficients(b, self.spectrum(c, length), length, counts[c], explicit)

    def frame(self, layouts, plan, trim=None):
        b = Bits()
        self.tiles(b, layouts)
        if self.channels > 1:
            b.put(0, 1)
        if self.flags & 0x80:
            b.put(self.rng.randrange(256), 8)
        if trim:
            b.put(1, 1)
            for value in trim:
                b.put(1 if value else 0, 1)
                if value:
                    b.put(value, log2(self.spf * 2))
        else:
            b.put(0, 1)
        done = [0] * self.channels
        current = [0] * self.channels
        reuse = [False] * self.channels
        while True:
            offset, length = self.spf, self.spf
            for c in range(self.channels):
                if offset > done[c]:
                    offset, length = done[c], layouts[c][current[c]]
            if offset == self.spf:
                break
            sub = [c for c in range(self.channels) if done[c] == offset and layouts[c][current[c]] == length]
            self.subframe(b, sub, length, current, reuse, plan)
            for c in sub:
                done[c] += length
                current[c] += 1
        body = b.bits
        size = self.size_bits + len(body) + 2
        head = Bits()
        head.put(size, self.size_bits)
        return head.bits + body + [0, 1]

    def packets(self, frames):
        out, sequence, pending = [], 0, []
        room = self.block * 8 - 6 - self.size_bits
        queue = list(frames)
        while queue or pending:
            b = Bits()
            b.put(sequence & 15, 4)
            b.put(0, 2)
            b.put(len(pending), self.size_bits)
            b.bits += pending
            pending = []
            while queue:
                left = self.block * 8 - len(b.bits)
                if not left:
                    break
                if len(queue[0]) <= left:
                    b.bits += queue.pop(0)
                else:
                    b.bits += queue[0][:left]
                    pending = queue.pop(0)[left:]
                    break
            b.bits += [0] * (self.block * 8 - len(b.bits))
            out.append(bytes(int("".join(map(str, b.bits[i:i + 8])), 2) for i in range(0, len(b.bits), 8)))
            sequence += 1
        return out

    def format(self, packets):
        extra = struct.pack("<HI8xHH", self.depth, self.mask, self.flags, 0)
        rate = self.block * len(packets) * self.rate // (self.spf * self.frames)
        return struct.pack("<HHIIHHH", 0x162, self.channels, self.rate, rate, self.block, self.depth if self.depth <= 16 else 24, len(extra)) + extra


def avi(path, stream, packets):
    def chunk(tag, body):
        return tag + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")

    def listed(tag, body):
        return chunk(b"LIST", tag + body)

    strf = stream.format(packets)
    rate = struct.unpack("<I", strf[8:12])[0]
    avih = struct.pack("<IIIIIIIIII4I", 20000, rate, 0, 0x10, 0, 0, 1, stream.block, 0, 0, 0, 0, 0, 0)
    strh = struct.pack("<4s4sIHHIIIIIIIIhhhh", b"auds", b"\0\0\0\0", 0, 0, 0, 0, stream.block, rate, 0, len(packets), stream.block, 0xFFFFFFFF, stream.block, 0, 0, 0, 0)
    header = listed(b"hdrl", chunk(b"avih", avih) + listed(b"strl", chunk(b"strh", strh) + chunk(b"strf", strf)))
    movi, index = b"", b""
    for p in packets:
        index += struct.pack("<4sIII", b"00wb", 0x10, 4 + len(movi), len(p))
        movi += chunk(b"00wb", p)
    body = b"AVI " + header + listed(b"movi", movi) + chunk(b"idx1", index)
    open(path, "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)


def whole(stream):
    return [[stream.spf] for _ in range(stream.channels)]


def split(stream, pattern):
    return [list(pattern[c % len(pattern)]) for c in range(stream.channels)]


def one_group(kind):
    return lambda sub: [(sub, kind)]


def pairs(sub):
    return [(sub[i:i + 2], "ms" if i % 4 == 0 else "none") for i in range(0, len(sub), 2)]


def lfe_apart(lfe, kind):
    def plan(sub):
        rest = [c for c in sub if c != lfe]
        if len(rest) == len(sub) or len(sub) <= 2:
            return [(sub, kind)]
        if len(rest) <= 1:
            return [(rest, "none"), ([lfe], "none")] if rest else [([lfe], "none")]
        return [(rest, kind), ([lfe], "none")]
    return plan


def make(rate, channels, mask, bits, flags, frames, layouts, plan, extras=None, seed=1):
    def build(block):
        s = Stream(rate, channels, mask, bits, flags, block, seed)
        s.frames = frames
        made = []
        for i in range(frames):
            p = {"step": -31}
            p.update(plan)
            p.update((extras or {}).get(i, {}))
            trim = p.pop("trim", None)
            made.append(s.frame(layouts(s, i), p, trim))
        return s, made

    _, made = build(4096)
    s, made = build(max(len(f) for f in made) // 8 * 5 // 4 + 16)
    return s, s.packets(made)


def main(folder):
    ffmpeg = os.environ.get("FFMPEG", "ffmpeg")
    jobs = [
        ("pro_mono", ".wma", dict(rate=44100, channels=1, mask=0x4, bits=16, flags=0x40, frames=14, layouts=lambda s, i: whole(s), plan={})),
        ("pro_stereo24", ".wma", dict(rate=48000, channels=2, mask=0x3, bits=24, flags=0x58, frames=16,
                                      layouts=lambda s, i: split(s, [[1024, 512, 256, 256], [2048]]) if i % 3 else split(s, [[512, 512, 1024]]),
                                      plan={"groups": one_group("ms"), "per_band": True},
                                      extras={1: {"trim": (300, 0)}, 4: {"fill": 2, "step": 31, "vectors": True}, 7: {"resend": True, "step": -32}, 15: {"trim": (0, 700)}})),
        ("pro_3ch20", ".wma", dict(rate=32000, channels=3, mask=0x7, bits=20, flags=0x50, frames=10,
                                   layouts=lambda s, i: split(s, [[1024, 1024]]) if i % 2 else whole(s), plan={"groups": one_group("default")})),
        ("pro_quad", ".mkv", dict(rate=44100, channels=4, mask=0x33, bits=16, flags=0x48, frames=10,
                                  layouts=lambda s, i: split(s, [[2048], [1024, 1024]]), plan={"groups": pairs})),
        ("pro_5ch", ".avi", dict(rate=48000, channels=5, mask=0x37, bits=16, flags=0x40, frames=10,
                                 layouts=lambda s, i: whole(s), plan={"groups": one_group("default"), "per_band": True})),
        ("pro_51side", ".wma", dict(rate=44100, channels=6, mask=0x60F, bits=16, flags=0xE0, frames=12,
                                    layouts=lambda s, i: split(s, [[512, 512, 1024], [512, 512, 1024], [2048], [128] * 4 + [512] * 3]),
                                    plan={"groups": lfe_apart(3, "default")}, extras={2: {"silent": 3}})),
        ("pro_61", ".wma", dict(rate=48000, channels=7, mask=0x70F, bits=16, flags=0x40, frames=10,
                                layouts=lambda s, i: whole(s), plan={"groups": lfe_apart(3, "rotate")})),
        ("pro_71", ".wma", dict(rate=48000, channels=8, mask=0x63F, bits=24, flags=0xD0, frames=12,
                                layouts=lambda s, i: split(s, [[1024, 1024]]) if i % 2 else whole(s),
                                plan={"groups": one_group("rotate")}, extras={0: {"trim": (100, 0)}, 3: {"groups": pairs}, 11: {"trim": (0, 333)}})),
        ("pro_nomask", ".wma", dict(rate=48000, channels=2, mask=0, bits=16, flags=0x42, frames=8,
                                    layouts=lambda s, i: whole(s), plan={"groups": one_group("none")})),
        ("pro_96k", ".wma", dict(rate=96000, channels=2, mask=0x3, bits=24, flags=0x54, frames=12,
                                 layouts=lambda s, i: split(s, [[1024, 512, 512]]), plan={"groups": one_group("ms")})),
    ]
    for name, kind, job in jobs:
        stream, packets = make(seed=len(name), **job)
        target = os.path.join(folder, name + kind)
        if kind == ".avi":
            avi(target, stream, packets)
        else:
            temporary = target + ".avi"
            avi(temporary, stream, packets)
            subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-y", "-fflags", "+bitexact", "-i", temporary, "-c", "copy", target], check=True)
            os.remove(temporary)
        print(name + kind, len(packets), "packets")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else HERE)
