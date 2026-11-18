import math
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TABLES = os.path.join(HERE, "..", "..", "..", "src", "vc1_tables.cpp")


def load():
    text = open(TABLES).read()
    found = {}
    for m in re.finditer(r"const \w+ (k\w+)((?:\[\d+\])+) = (\{.*?\});", text, re.S):
        found[m.group(1)] = [int(x, 0) for x in re.findall(r"0[xX][0-9a-fA-F]+|\d+", m.group(3))]
    return found


T = load()
DC = [[T["kDcTables"][(i * 2 + j) * 240:(i * 2 + j + 1) * 240] for j in range(2)] for i in range(2)]
MB_I = T["kMbITable"]
MV_CODES, MV_BITS = T["kMvDiffCodes"], T["kMvDiffBits"]
DC_SCALE = T["kWmv3DcScaleTable"]
PQUANT = T["kPquantTable"]
DCPRED = [0xFFFF, 1024, 512, 341, 256, 205, 171, 146, 128, 114, 102, 93, 85, 79, 73, 68, 64, 60, 57, 54, 51, 49, 47, 45, 43, 41, 39, 38, 37, 35, 34, 33]


class Bits:
    def __init__(self):
        self.bits = []

    def put(self, value, count):
        for i in reversed(range(count)):
            self.bits.append((value >> i) & 1)

    def code(self, pair):
        self.put(pair[0], pair[1])

    def data(self):
        bits = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


class Stream:
    def __init__(self, simple, width, height):
        self.simple = simple
        self.mbw, self.mbh = (width + 15) // 16, (height + 15) // 16
        self.width, self.height = width, height
        self.rangered = 0 if simple else 1
        self.max_b = 0 if simple else 1
        self.loop = 0 if simple else 1
        self.overlap = 1
        self.dquant = 0

    def sequence(self):
        b = Bits()
        b.put(0 if self.simple else 1, 2)
        b.put(0, 2)
        b.put(0, 3)
        b.put(0, 5)
        b.put(self.loop, 1)
        b.put(0, 1)
        b.put(0, 1)
        b.put(1, 1)
        b.put(1, 1)
        b.put(0, 1)
        b.put(self.dquant, 2)
        b.put(0, 1)
        b.put(0, 1)
        b.put(self.overlap, 1)
        b.put(0, 1)
        b.put(self.rangered, 1)
        b.put(self.max_b, 3)
        b.put(0, 2)
        b.put(0, 1)
        b.put(1, 1)
        return b.data()

    def head(self, b, kind, pqindex, reduced=0):
        b.put(0, 2)
        if self.rangered:
            b.put(reduced, 1)
        if kind == "P":
            b.put(1, 1)
        else:
            b.put(0, 1)
            if self.max_b:
                b.put(1 if kind == "I" else 0, 1)
        if kind == "B":
            b.put(0, 3)
        if kind == "I":
            b.put(0, 7)
        b.put(pqindex, 5)
        if pqindex < 9:
            b.put(0, 1)

    def intra(self, phase, pqindex, reduced=0):
        b = Bits()
        self.head(b, "I", pqindex, reduced)
        b.put(0, 1)
        b.put(0, 1)
        b.put(0, 1)
        pq = PQUANT[pqindex]
        scale = DC_SCALE[pq]
        edge = pq < 9 or not self.overlap
        luma = {}
        chroma = [{}, {}]

        base = DCPRED[scale] if edge else 0

        def target(x, y, plane):
            value = 45 * math.sin((x + phase) * 0.35 + plane) + 35 * math.cos((y - phase) * 0.45 - plane)
            return int(round(value))

        def predict(grid, x, y, own):
            special = DCPRED[scale] if edge else 0
            a = grid.get((x, y - 1), 0)
            b_ = grid.get((x - 1, y - 1), 0)
            c = grid.get((x - 1, y), 0)
            if own[0]:
                a = b_ = special
            if own[1]:
                b_ = c = special
            return c if abs(a - b_) <= abs(b_ - c) else a

        for my in range(self.mbh):
            for mx in range(self.mbw):
                b.code((MB_I[0], MB_I[1]))
                b.put(0, 1)
                for n in range(6):
                    if n < 4:
                        x, y = 2 * mx + (n & 1), 2 * my + (n >> 1)
                        grid = luma
                        own = (my == 0 and n not in (2, 3), mx == 0 and n not in (1, 3))
                    else:
                        x, y = mx, my
                        grid = chroma[n - 4]
                        own = (my == 0, mx == 0)
                    value = base + target(x if n < 4 else 2 * x, y if n < 4 else 2 * y, n - 3 if n >= 4 else 0) // (2 if n >= 4 else 1)
                    diff = value - predict(grid, x, y, own)
                    grid[(x, y)] = value
                    table = DC[0][1 if n >= 4 else 0]
                    magnitude = abs(diff)
                    assert magnitude < 119
                    b.put(table[magnitude * 2], table[magnitude * 2 + 1])
                    if diff:
                        b.put(1 if diff < 0 else 0, 1)
        return b.data()

    def plane(self, b, invert, mode, rows):
        b.put(invert, 1)
        if mode == "rowskip":
            b.put(2, 3)
            for row in rows:
                if any(row):
                    b.put(1, 1)
                    for v in row:
                        b.put(v, 1)
                else:
                    b.put(0, 1)
        elif mode == "colskip":
            b.put(3, 3)
            for column in zip(*rows):
                if any(column):
                    b.put(1, 1)
                    for v in column:
                        b.put(v, 1)
                else:
                    b.put(0, 1)
        else:
            b.put(0, 4)

    def predicted(self, pqindex, motion, mode, light=None, reduced=0):
        b = Bits()
        self.head(b, "P", pqindex, reduced)
        lowquant = 0 if PQUANT[pqindex] > 12 else 1
        modes = [["bilinear", "1mv", "hpel", "ic", "mixed"], ["1mv", "mixed", "hpel", "ic", "bilinear"]][lowquant]
        if light:
            b.put(1, modes.index("ic") + 1)
            second = [["bilinear", "1mv", "hpel", "mixed"], ["1mv", "mixed", "hpel", "bilinear"]][lowquant].index(mode)
            b.put(1, second + 1) if second < 3 else b.put(0, 3)
            b.put(light[0], 6)
            b.put(light[1], 6)
        else:
            index = modes.index(mode)
            b.put(1, index + 1) if index < 4 else b.put(0, 4)
        rows = [[1 if (x, y) == (0, 0) else 0 for x in range(self.mbw)] for y in range(self.mbh)]
        self.plane(b, 1, "colskip", rows)
        b.put(0, 2)
        b.put(0, 2)
        b.put(0, 1)
        b.put(0, 1)
        quarter = mode == "1mv"
        index = 6 * motion[1][0] + motion[0][0]
        assert 1 <= index <= 34
        b.put(MV_CODES[index - 1], MV_BITS[index - 1])
        for (part, value) in motion:
            size = [0, 2, 3, 4, 5, 8][part] - (1 if not quarter and part == 5 else 0)
            b.put(value, size)
        return b.data()

    def edge_intra(self, pqindex, absolute, dc):
        b = Bits()
        self.head(b, "P", pqindex)
        modes = ["1mv", "mixed", "hpel", "ic", "bilinear"] if PQUANT[pqindex] <= 12 else ["bilinear", "1mv", "hpel", "ic", "mixed"]
        b.put(1, modes.index("1mv") + 1)
        rows = [[1 if (x, y) == (0, 0) else 0 for x in range(self.mbw)] for y in range(self.mbh)]
        self.plane(b, 1, "colskip", rows)
        b.put(0, 2)
        b.put(0, 2)
        if self.dquant == 1:
            b.put(1, 1)
            b.put(0, 2)
        b.put(7, 3)
        b.put(absolute, 5)
        b.put(0, 1)
        b.put(0, 1)
        b.put(MV_CODES[35], MV_BITS[35])
        b.put(0, 1)
        for n, diff in enumerate([dc, 0, 0, 0, dc // 2, -dc // 2]):
            table = DC[0][1 if n >= 4 else 0]
            b.put(table[abs(diff) * 2], table[abs(diff) * 2 + 1])
            if diff:
                b.put(1 if diff < 0 else 0, 1)
        return b.data()

    def bidirectional(self, pqindex, interpolated):
        b = Bits()
        self.head(b, "B", pqindex)
        b.put(1, 1)
        everything = [[0] * self.mbw for _ in range(self.mbh)]
        self.plane(b, 0 if interpolated else 1, "rowskip", everything)
        self.plane(b, 1, "rowskip", everything)
        b.put(0, 2)
        b.put(0, 2)
        b.put(0, 1)
        b.put(0, 1)
        if interpolated:
            for _ in range(self.mbw * self.mbh):
                b.put(3, 2)
        return b.data()


def avi(path, width, height, sequence, frames, rate=25):
    def chunk(tag, body):
        return tag + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")

    def listed(tag, body):
        return chunk(b"LIST", tag + body)

    largest = max(len(f) for _, f in frames)
    avih = struct.pack("<IIIIIIIIII4I", 1000000 // rate, 0, 0, 0x10, len(frames), 0, 1, largest, width, height, 0, 0, 0, 0)
    strh = struct.pack("<4s4sIHHIIIIIIIIhhhh", b"vids", b"WMV3", 0, 0, 0, 0, 1, rate, 0, len(frames), largest, 0xFFFFFFFF, 0, 0, 0, width, height)
    strf = struct.pack("<IiiHH4sIiiII", 40 + len(sequence), width, height, 1, 24, b"WMV3", width * height * 3, 0, 0, 0, 0) + sequence
    header = listed(b"hdrl", chunk(b"avih", avih) + listed(b"strl", chunk(b"strh", strh) + chunk(b"strf", strf)))
    movi = b""
    index = b""
    for key, frame in frames:
        index += struct.pack("<4sIII", b"00dc", 0x10 if key else 0, 4 + len(movi), len(frame))
        movi += chunk(b"00dc", frame)
    body = b"AVI " + header + listed(b"movi", movi) + chunk(b"idx1", index)
    open(path, "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)


def seed(path, sequence, frames):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(bytes([0]) + sequence + b"".join(struct.pack(">H", len(f)) + f for _, f in frames))


def main(folder):
    main_profile = Stream(False, 176, 144)
    frames = [
        (True, main_profile.intra(0, 12)),
        (False, main_profile.predicted(12, ((2, 4), (2, 0)), "1mv")),
        (False, main_profile.bidirectional(12, False)),
        (False, main_profile.predicted(13, ((1, 2), (3, 3)), "1mv", light=(36, 5))),
        (False, main_profile.bidirectional(13, True)),
        (False, main_profile.predicted(12, ((2, 1), (1, 0)), "hpel", reduced=1)),
        (False, main_profile.bidirectional(12, False)),
        (True, main_profile.intra(3, 14, reduced=1)),
        (False, main_profile.predicted(10, ((3, 5), (2, 6)), "1mv")),
        (False, main_profile.bidirectional(10, False)),
        (False, main_profile.predicted(12, ((1, 1), (1, 3)), "bilinear", light=(20, 40))),
        (False, main_profile.bidirectional(12, True)),
    ]
    avi(os.path.join(folder, "main_bframes.avi"), 176, 144, main_profile.sequence(), frames)
    seed(os.path.join(folder, "..", "fuzz", "vc1", "main_bframes.bin"), main_profile.sequence(), frames)
    simple = Stream(True, 160, 112)
    frames = [(True, simple.intra(1, 6))]
    for i, (motion, mode) in enumerate([(((1, 2), (1, 1)), "bilinear"), (((2, 3), (1, 0)), "hpel"), (((2, 7), (2, 5)), "1mv"), (((1, 0), (3, 9)), "1mv")]):
        frames.append((False, simple.predicted(7 + i, motion, mode)))
    frames.append((True, simple.intra(5, 3)))
    frames.append((False, simple.predicted(5, ((2, 2), (2, 2)), "1mv")))
    avi(os.path.join(folder, "simple.avi"), 160, 112, simple.sequence(), frames)
    seed(os.path.join(folder, "..", "fuzz", "vc1", "simple.bin"), simple.sequence(), frames)


def edges(folder):
    for dquant in (1, 2):
        stream = Stream(False, 64, 48)
        stream.dquant = dquant
        frames = [(True, stream.intra(2, 12)), (False, stream.edge_intra(12, 20, 24))]
        avi(os.path.join(folder, f"dquant{dquant}.avi"), 64, 48, stream.sequence(), frames)


if __name__ == "__main__":
    folder = sys.argv[1] if len(sys.argv) > 1 else HERE
    if sys.argv[2:] == ["dquant"]:
        edges(folder)
    else:
        main(folder)
