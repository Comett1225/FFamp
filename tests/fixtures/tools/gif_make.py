import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def palette():
    colours = [(0, 0, 0), (255, 255, 255), (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (0, 255, 255), (255, 0, 255)]
    for i in range(8, 16):
        colours.append((i * 16, 255 - i * 12, (i * 37) % 256))
    return colours


def block(width, height, step, kind):
    rows = []
    for y in range(height):
        row = []
        for x in range(width):
            if kind == "bars":
                row.append(1 + ((x // 6 + step) % 15))
            elif kind == "ball":
                inside = (x - width // 2) ** 2 + (y - height // 2) ** 2 < (min(width, height) // 2 - 1) ** 2
                row.append(2 + step % 6 if inside else 0)
            else:
                row.append(1 + ((x // 4 + y // 4 + step) % 15))
        rows.append(row)
    return rows


def scenes():
    opaque = {"name": "opaque.gif", "width": 64, "height": 48, "frames": []}
    for step, delay in enumerate([10, 0, 1, 2, 7, 25, 4, 10]):
        opaque["frames"].append({"x": 0, "y": 0, "pixels": block(64, 48, step, "bars"), "clear": None, "dispose": 1, "delay": delay})
    alpha = {"name": "alpha.gif", "width": 37, "height": 23, "frames": []}
    alpha["frames"].append({"x": 0, "y": 0, "pixels": block(37, 23, 0, "checks"), "clear": None, "dispose": 1, "delay": 8})
    for step in range(1, 9):
        alpha["frames"].append({"x": (step * 4) % 22, "y": (step * 3) % 9, "pixels": block(14, 13, step, "ball"), "clear": 0, "dispose": [1, 2, 3][step % 3], "delay": 4 + step})
    still = {"name": "still.gif", "width": 20, "height": 10, "frames": [{"x": 0, "y": 0, "pixels": block(20, 10, 3, "bars"), "clear": None, "dispose": 0, "delay": 10}]}
    woven = {"name": "woven.gif", "width": 30, "height": 21, "frames": []}
    for step in range(4):
        woven["frames"].append({"x": step, "y": step, "pixels": block(24, 17, step, "checks"), "clear": 3 if step else None, "dispose": 1, "delay": 6, "woven": True, "own": step % 2 == 1})
    partial = {"name": "partial.gif", "width": 30, "height": 21, "frames": []}
    for step in range(5):
        partial["frames"].append({"x": 2 + step, "y": 1 + step, "pixels": block(20, 12, step, "checks"), "clear": None, "dispose": [1, 2, 1, 3, 1][step], "delay": 6})
    return [opaque, alpha, still, woven, partial]


def own_palette():
    return [(255 - r, g, 255 - b) for r, g, b in palette()]


def woven_order(height):
    return [y for first, step in ((0, 8), (4, 8), (2, 4), (1, 2)) for y in range(first, height, step)]


def packed(codes_bits):
    out = bytearray()
    value = 0
    bits = 0
    for code, size in codes_bits:
        value |= code << bits
        bits += size
        while bits >= 8:
            out.append(value & 255)
            value >>= 8
            bits -= 8
    if bits:
        out.append(value & 255)
    return bytes(out)


def lzw(indices, minimum):
    clear = 1 << minimum
    stop = clear + 1
    table = {(i,): i for i in range(clear)}
    size = minimum + 1
    following = stop + 1
    out = [(clear, size)]
    current = ()
    for index in indices:
        longer = current + (index,)
        if longer in table:
            current = longer
            continue
        out.append((table[current], size))
        if following < 4096:
            table[longer] = following
            following += 1
            if following > (1 << size) and size < 12:
                size += 1
        else:
            out.append((clear, size))
            table = {(i,): i for i in range(clear)}
            size = minimum + 1
            following = stop + 1
        current = (index,)
    out.append((table[current], size))
    out.append((stop, size))
    return packed(out)


def write(scene, path):
    colours = palette()
    data = bytearray(b"GIF89a")
    data += struct.pack("<HHBBB", scene["width"], scene["height"], 0xF3, 0, 0)
    for r, g, b in colours:
        data += bytes([r, g, b])
    data += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00"
    for frame in scene["frames"]:
        clear = frame["clear"]
        data += b"\x21\xF9\x04" + bytes([(frame["dispose"] << 2) | (1 if clear is not None else 0)]) + struct.pack("<H", frame["delay"]) + bytes([clear or 0, 0])
        rows = frame["pixels"]
        data += b"\x2C" + struct.pack("<HHHHB", frame["x"], frame["y"], len(rows[0]), len(rows), (0x40 if frame.get("woven") else 0) | (0x83 if frame.get("own") else 0))
        if frame.get("own"):
            for r, g, b in own_palette():
                data += bytes([r, g, b])
        if frame.get("woven"):
            rows = [rows[y] for y in woven_order(len(rows))]
        body = lzw([v for row in rows for v in row], 4)
        data += bytes([4])
        for at in range(0, len(body), 255):
            piece = body[at:at + 255]
            data += bytes([len(piece)]) + piece
        data += b"\x00"
    data += b"\x3B"
    with open(path, "wb") as out:
        out.write(data)


def composite(scene):
    colours = palette()
    width, height = scene["width"], scene["height"]
    canvas = [[(0, 0, 0, 0)] * width for _ in range(height)]
    shown = []
    for frame in scene["frames"]:
        saved = [row[:] for row in canvas] if frame["dispose"] == 3 else None
        rows = frame["pixels"]
        for y, row in enumerate(rows):
            for x, index in enumerate(row):
                if index == frame["clear"]:
                    continue
                cx, cy = frame["x"] + x, frame["y"] + y
                if 0 <= cx < width and 0 <= cy < height:
                    canvas[cy][cx] = (own_palette() if frame.get("own") else colours)[index] + (255,)
        shown.append([row[:] for row in canvas])
        if frame["dispose"] == 2:
            for y in range(len(rows)):
                for x in range(len(rows[0])):
                    cx, cy = frame["x"] + x, frame["y"] + y
                    if 0 <= cx < width and 0 <= cy < height:
                        canvas[cy][cx] = (0, 0, 0, 0)
        elif saved is not None:
            canvas = saved
    return shown


def delays(scene):
    return [100 if frame["delay"] <= 1 else frame["delay"] * 10 for frame in scene["frames"]]


if __name__ == "__main__":
    for scene in scenes():
        write(scene, os.path.join(HERE, scene["name"]))
