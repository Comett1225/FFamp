import sys


def units(data):
    starts = []
    i = data.find(b"\0\0\1")
    while i >= 0:
        starts.append(i)
        i = data.find(b"\0\0\1", i + 3)
    return [data[a:b] for a, b in zip(starts, starts[1:] + [len(data)])]


def fields(source, target, first="top"):
    top = 1 if first == "top" else 2
    out = bytearray()
    field = -1
    leading = True
    for unit in units(open(source, "rb").read()):
        unit = bytearray(unit)
        code = unit[3]
        if 0x01 <= code <= 0xAF:
            leading = True
        elif code == 0x00:
            field += 1
            leading = False
            unit[4] = 0
            unit[5] &= 0x3F
        elif leading and field % 2 == 0 and code != 0xB7:
            continue
        if code == 0xB3:
            height = ((unit[5] & 15) << 8 | unit[6]) * 2
            unit[5] = (unit[5] & 0xF0) | height >> 8
            unit[6] = height & 255
            unit[7] = (unit[7] & 0xF0) | 3
        elif code == 0xB5 and unit[4] >> 4 == 1:
            unit[5] &= 0xF7
        elif code == 0xB5 and unit[4] >> 4 == 8:
            unit[6] = (unit[6] & 0xFC) | (3 - top if field % 2 else top)
            unit[7] &= 0x3C
            unit[8] &= 0x7F
        out += unit
    open(target, "wb").write(out)


def nohuffman(source, target):
    data = open(source, "rb").read()
    out = bytearray()
    for picture in data.split(b"\xff\xd8")[1:]:
        out += b"\xff\xd8"
        at = 0
        while at + 4 <= len(picture) and picture[at] == 0xFF:
            marker = picture[at + 1]
            length = picture[at + 2] << 8 | picture[at + 3]
            if marker == 0xDA:
                break
            if marker != 0xC4:
                out += picture[at:at + 2 + length]
            at += 2 + length
        out += picture[at:]
    open(target, "wb").write(out)


if __name__ == "__main__":
    {"fields": fields, "nohuffman": nohuffman}[sys.argv[1]](*sys.argv[2:])
