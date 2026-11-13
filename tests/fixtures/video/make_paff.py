import sys

import numpy

WIDTH, HEIGHT, FRAMES = 96, 64, 15


class Bits:
    def __init__(self):
        self.bits = []

    def u(self, count, value):
        self.bits += [(value >> (count - 1 - i)) & 1 for i in range(count)]

    def ue(self, value):
        value += 1
        size = value.bit_length()
        self.u(size - 1, 0)
        self.u(size, value)

    def se(self, value):
        self.ue(2 * value - 1 if value > 0 else -2 * value)

    def align(self):
        while len(self.bits) % 8:
            self.bits.append(0)

    def raw(self, data):
        for byte in data:
            self.u(8, int(byte))

    def trailing(self):
        self.bits.append(1)
        self.align()

    def nal(self, ref, kind):
        body = numpy.packbits(numpy.array(self.bits, dtype=numpy.uint8)).tobytes()
        out = bytearray([0, 0, 0, 1, (ref << 5) | kind])
        zeros = 0
        for byte in body:
            if zeros >= 2 and byte <= 3:
                out.append(3)
                zeros = 0
            out.append(byte)
            zeros = zeros + 1 if byte == 0 else 0
        return bytes(out)


def sequence():
    b = Bits()
    b.u(8, 77)
    b.u(8, 0)
    b.u(8, 30)
    b.ue(0)
    b.ue(0)
    b.ue(0)
    b.ue(4)
    b.ue(1)
    b.u(1, 0)
    b.ue(WIDTH // 16 - 1)
    b.ue(HEIGHT // 32 - 1)
    b.u(1, 0)
    b.u(1, 0)
    b.u(1, 1)
    b.u(1, 0)
    b.u(1, 0)
    b.trailing()
    return b.nal(3, 7)


def picture():
    b = Bits()
    b.ue(0)
    b.ue(0)
    b.u(1, 0)
    b.u(1, 0)
    b.ue(0)
    b.ue(0)
    b.ue(0)
    b.u(1, 0)
    b.u(2, 0)
    b.se(0)
    b.se(0)
    b.se(0)
    b.u(1, 1)
    b.u(1, 0)
    b.u(1, 0)
    b.trailing()
    return b.nal(3, 8)


def field(frame, bottom, planes):
    first = frame == 0 and not bottom
    b = Bits()
    b.ue(0)
    b.ue(7 if planes else 5)
    b.ue(0)
    b.u(4, frame % 16)
    b.u(1, 1)
    b.u(1, 1 if bottom else 0)
    if first:
        b.ue(0)
    b.u(8, (2 * frame + (1 if bottom else 0)) % 256)
    if not planes:
        b.u(1, 0)
        b.u(1, 0)
    if first:
        b.u(1, 0)
        b.u(1, 0)
    else:
        b.u(1, 0)
    b.se(0)
    b.ue(1)
    columns, rows = WIDTH // 16, HEIGHT // 32
    if planes:
        luma, blue, red = planes
        for row in range(rows):
            for column in range(columns):
                b.ue(25)
                b.align()
                b.raw(luma[row * 16:row * 16 + 16, column * 16:column * 16 + 16].reshape(-1))
                b.raw(blue[row * 8:row * 8 + 8, column * 8:column * 8 + 8].reshape(-1))
                b.raw(red[row * 8:row * 8 + 8, column * 8:column * 8 + 8].reshape(-1))
    else:
        b.ue(columns * rows)
    b.trailing()
    return b.nal(3, 5 if first else 1)


def content(frame, bottom):
    y, x = numpy.mgrid[0:HEIGHT // 2, 0:WIDTH]
    line = 2 * y + (1 if bottom else 0)
    shift = frame * 4 + (2 if bottom else 0)
    luma = 40 + ((x + shift) * 3 + line * 2) % 160
    luma[(x + shift) // 12 % 2 == line // 12 % 2] += 30
    cy, cx = numpy.mgrid[0:HEIGHT // 4, 0:WIDTH // 2]
    blue = 60 + ((cx * 5 + frame * 9 + cy * 3) % 130)
    red = 70 + ((cy * 11 + shift * 2 + cx) % 120)
    return luma.astype(numpy.uint8), blue.astype(numpy.uint8), red.astype(numpy.uint8)


out = bytearray(sequence() + picture())
for frame in range(FRAMES):
    kind = frame % 3
    top = content(frame, False) if kind != 2 else None
    low = content(frame, True) if kind == 1 else None
    out += field(frame, False, top)
    out += field(frame, True, low)
open(sys.argv[1], "wb").write(out)
