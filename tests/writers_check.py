import hashlib
import os
import struct
import subprocess
import sys

import numpy

FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
FFPROBE = os.environ.get("FFPROBE", "ffprobe")


def fail(text):
    print("  " + text)
    sys.exit(1)


def need(condition, text):
    if not condition:
        fail(text)


def signal(path, kind, rate, channels, frames):
    rng = numpy.random.default_rng(11)
    t = numpy.arange(frames) / rate
    lanes = []
    for c in range(channels):
        if kind == "music":
            lane = sum(0.25 / (h + 1) * numpy.sin(2 * numpy.pi * (220 + 55 * c) * (h + 1) * t + h) * numpy.exp(-((t * 2) % 1.0) * (1 + h)) for h in range(6))
            lane = lane + 0.002 * rng.standard_normal(frames)
        elif kind == "noise":
            lane = 0.9 * (rng.random(frames) * 2 - 1)
        elif kind == "silence":
            lane = numpy.zeros(frames)
        elif kind == "steady":
            lane = numpy.full(frames, 0.25)
        elif kind == "coarse":
            lane = numpy.round(0.4 * numpy.sin(2 * numpy.pi * 300 * t + c) * 32767) / 32768
        else:
            lane = 0.5 * numpy.sin(2 * numpy.pi * 440 * t + c) + 0.2 * numpy.sin(2 * numpy.pi * 3000 * t)
        lanes.append(lane)
    x = numpy.stack(lanes, 1).astype(numpy.float32)
    if kind == "music" and frames > 200:
        x[100, 0] = 1.5
        x[101, -1] = -1.5
        x[102, 0] = 0.5 / 32768
        x[103, 0] = 1.5 / 32768
        x[104, 0] = -0.5 / 32768
        x[105, 0] = -0.7 / 2147483648
    x.tofile(path)


def fixed(path):
    x = numpy.fromfile(path, dtype=numpy.float32)
    scaled = (x * numpy.float32(2147483648.0)).astype(numpy.float64)
    return numpy.clip(numpy.trunc(scaled), -2147483648, 2147483647).astype(numpy.int64)


def expected(path, kind):
    if kind == "f":
        return open(path, "rb").read()
    whole = fixed(path)
    if kind == "16":
        return numpy.minimum((whole + 0x8000) >> 16, 32767).astype("<i2").tobytes()
    deep = (whole >> 8).astype("<i4").tobytes()
    return numpy.frombuffer(deep, dtype=numpy.uint8).reshape(-1, 4)[:, :3].tobytes()


def wav(path, source, rate, channels, kind, wide):
    data = open(path, "rb").read()
    want = expected(source, kind)
    bits = {"16": 16, "24": 24, "f": 32}[kind]
    frames = len(want) // (channels * bits // 8)
    magic, riff_size, wave = struct.unpack("<4sI4s", data[:12])
    need(wave == b"WAVE" and magic == (b"RF64" if wide else b"RIFF"), "wrong RIFF magic")
    at = 12
    chunks = {}
    order = []
    sizes = {}
    while at + 8 <= len(data):
        name, size = struct.unpack("<4sI", data[at:at + 8])
        if name == b"data" and size == 0xFFFFFFFF:
            size = sizes["data"]
        chunks[name] = data[at + 8:at + 8 + size]
        order.append(name)
        if name == b"ds64":
            riff64, data64, samples64, tables = struct.unpack("<QQQI", chunks[name][:28])
            sizes["data"] = data64
            need(riff64 == len(data) - 8, "ds64 RIFF size is wrong")
            need(samples64 == frames, "ds64 sample count is wrong")
            need(data64 == len(want), "ds64 data size is wrong")
        at += 8 + size + (size & 1)
    need(at == len(data), "chunks do not end at the end of the file")
    need(len(data) % 2 == 0, "file length is odd")
    if wide:
        need(riff_size == 0xFFFFFFFF and order[0] == b"ds64", "RF64 needs ds64 first and a full RIFF size field")
    else:
        need(riff_size == len(data) - 8, "RIFF size is wrong")
        need(b"ds64" not in chunks, "plain RIFF must not carry ds64")
    fmt = chunks[b"fmt "]
    tag, ch, sr, byte_rate, align, depth = struct.unpack("<HHIIHH", fmt[:16])
    extensible = channels > 2 or kind == "24"
    need(tag == (0xFFFE if extensible else 3 if kind == "f" else 1), "format tag is wrong")
    need((ch, sr, depth, align, byte_rate) == (channels, rate, bits, channels * bits // 8, rate * channels * bits // 8), "fmt fields are wrong")
    if extensible:
        size, valid, mask, sub = struct.unpack("<HHIH", fmt[16:26])
        need(len(fmt) == 40 and size == 22 and valid == bits and sub == (3 if kind == "f" else 1), "extensible fmt is wrong")
        need(fmt[26:] == bytes.fromhex("000000001000800000aa00389b71"), "sub format GUID is wrong")
        need(bin(mask).count("1") == channels, "channel mask does not match the channel count")
    if kind == "f":
        need(struct.unpack("<I", chunks[b"fact"])[0] == (0xFFFFFFFF if wide else frames), "fact chunk is wrong")
    need(chunks[b"data"] == want, "samples differ from the expected conversion")
    print(f"  wav {os.path.basename(path)}: {frames} frames, bit-exact")


def crc8(data):
    value = 0
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = ((value << 1) ^ 0x07) & 0xFF if value & 0x80 else (value << 1) & 0xFF
    return value


CRC16 = []
for i in range(256):
    v = i << 8
    for _ in range(8):
        v = ((v << 1) ^ 0x8005) & 0xFFFF if v & 0x8000 else (v << 1) & 0xFFFF
    CRC16.append(v)


def flac_frame_head(data, at):
    need(data[at] == 0xFF and data[at + 1] == 0xF8, f"no frame sync at {at}")
    size_code, rate_code = data[at + 2] >> 4, data[at + 2] & 15
    assignment, depth_code = data[at + 3] >> 4, (data[at + 3] >> 1) & 7
    need(data[at + 3] & 1 == 0 and size_code != 0 and rate_code != 15 and assignment <= 10 and depth_code != 3, "reserved frame header value")
    first = data[at + 4]
    extra = 0
    while first & (0x80 >> extra):
        extra += 1
    number = first & (0x7F >> extra) if extra else first
    for i in range(1, extra):
        need(data[at + 4 + i] & 0xC0 == 0x80, "bad frame number coding")
        number = (number << 6) | (data[at + 4 + i] & 0x3F)
    here = at + 4 + max(extra, 1)
    if size_code == 6:
        block = data[here] + 1
        here += 1
    elif size_code == 7:
        block = struct.unpack(">H", data[here:here + 2])[0] + 1
        here += 2
    else:
        block = 192 if size_code == 1 else 576 << (size_code - 2) if size_code <= 5 else 256 << (size_code - 8)
    here += {12: 1, 13: 2, 14: 2}.get(rate_code, 0)
    need(crc8(data[at:here]) == data[here], f"frame header CRC-8 is wrong at {at}")
    return number, block, here + 1, assignment


def flac(path, source, rate, channels, bits):
    data = open(path, "rb").read()
    want = expected(source, str(bits))
    frames = len(want) // (channels * bits // 8)
    need(data[:4] == b"fLaC", "no fLaC marker")
    at = 4
    kinds = []
    info = None
    points = []
    while True:
        last, kind = data[at] >> 7, data[at] & 0x7F
        size = int.from_bytes(data[at + 1:at + 4], "big")
        body = data[at + 4:at + 4 + size]
        need(len(body) == size, "metadata block runs past the end")
        kinds.append(kind)
        if kind == 0:
            info = body
        if kind == 3:
            need(size % 18 == 0, "seek table size is not a multiple of 18")
            points = [struct.unpack(">QQH", body[i:i + 18]) for i in range(0, size, 18)]
        if kind == 4:
            vendor = struct.unpack("<I", body[:4])[0]
            need(struct.unpack("<I", body[4 + vendor:8 + vendor])[0] == 0 and 8 + vendor == size, "comment block is wrong")
        at += 4 + size
        if last:
            break
    need(kinds[0] == 0 and len(info) == 34, "STREAMINFO must come first with 34 bytes")
    low_block, high_block = struct.unpack(">HH", info[:4])
    low_frame, high_frame = int.from_bytes(info[4:7], "big"), int.from_bytes(info[7:10], "big")
    packed = int.from_bytes(info[10:18], "big")
    need(packed >> 44 == rate, "STREAMINFO rate is wrong")
    need(((packed >> 41) & 7) + 1 == channels and ((packed >> 36) & 31) + 1 == bits, "STREAMINFO layout is wrong")
    need(packed & 0xFFFFFFFFF == frames, f"STREAMINFO total samples {packed & 0xFFFFFFFFF} is not {frames}")
    need(info[18:] == hashlib.md5(want).digest(), "STREAMINFO MD5 does not match the samples")
    first_frame = at
    starts = {}
    sizes = []
    total = 0
    count = 0
    while at < len(data):
        number, block, body_at, assignment = flac_frame_head(data, at)
        need(number == count, "frame numbers are not consecutive")
        need(assignment < channels or (channels == 2 and assignment >= 8), "channel assignment does not fit the channel count")
        starts[total] = (at - first_frame, block)
        crc = 0
        for byte in data[at:body_at]:
            crc = ((crc << 8) & 0xFFFF) ^ CRC16[(crc >> 8) ^ byte]
        end = body_at
        found = False
        while end + 2 <= len(data):
            if (end + 2 == len(data) or (data[end + 2] == 0xFF and data[end + 3] == 0xF8)) and crc == (data[end] << 8 | data[end + 1]):
                found = True
                break
            crc = ((crc << 8) & 0xFFFF) ^ CRC16[(crc >> 8) ^ data[end]]
            end += 1
        need(found, f"frame {count} has no matching CRC-16")
        sizes.append(end + 2 - at)
        need(block == low_block or total + block == frames, "only the last block may be short")
        total += block
        count += 1
        at = end + 2
    need(total == frames, f"frames hold {total} samples, expected {frames}")
    need(low_block == high_block and low_block <= 4608, "block size limits are wrong")
    need((low_frame, high_frame) == (min(sizes), max(sizes)), "STREAMINFO frame size limits are wrong")
    real = [p for p in points if p[0] != 0xFFFFFFFFFFFFFFFF]
    need(points[:len(real)] == real and real == sorted(real), "seek points are not sorted with the fillers last")
    need(len(real) > 0, "no seek points")
    for sample, offset, block in real:
        need(starts.get(sample) == (offset, block), f"seek point for sample {sample} does not hit a frame")
    print(f"  flac {os.path.basename(path)}: {count} frames, {frames} samples, {len(real)} seek points, {len(data)} bytes ({100 * len(data) / max(len(want), 1):.1f} % of PCM)")


OGG_CRC = []
for i in range(256):
    v = i << 24
    for _ in range(8):
        v = ((v << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if v & 0x80000000 else (v << 1) & 0xFFFFFFFF
    OGG_CRC.append(v)


def opus_samples(packet):
    need(len(packet) >= 1, "empty Opus packet")
    config = packet[0] >> 3
    if config < 12:
        size = [480, 960, 1920, 2880][config & 3]
    elif config < 16:
        size = [480, 960][config & 1]
    else:
        size = [120, 240, 480, 960][config & 3]
    code = packet[0] & 3
    return size * (1 if code == 0 else 2 if code < 3 else packet[1] & 63)


def ogg(path, frames, channels):
    data = open(path, "rb").read()
    at = 0
    pages = []
    while at < len(data):
        need(data[at:at + 4] == b"OggS" and data[at + 4] == 0, f"no page at {at}")
        flags = data[at + 5]
        granule, serial, sequence, crc, count = struct.unpack("<qIIIB", data[at + 6:at + 27])
        lacing = data[at + 27:at + 27 + count]
        size = 27 + count + sum(lacing)
        page = bytearray(data[at:at + size])
        need(len(page) == size, "page runs past the end")
        page[22:26] = b"\0\0\0\0"
        value = 0
        for byte in page:
            value = ((value << 8) & 0xFFFFFFFF) ^ OGG_CRC[(value >> 24) ^ byte]
        need(value == crc, f"page {len(pages)} CRC is wrong")
        need(lacing[-1] != 255 and not flags & 1, "a packet crosses a page")
        packets = []
        body = at + 27 + count
        length = 0
        for lace in lacing:
            length += lace
            if lace < 255:
                packets.append(data[body:body + length])
                body += length
                length = 0
        pages.append((flags, granule, serial, sequence, packets))
        at += size
    need(len(pages) >= 3, "too few pages")
    need(all(p[2] == pages[0][2] for p in pages), "serial number changes")
    need([p[3] for p in pages] == list(range(len(pages))), "page sequence numbers are not consecutive")
    need(pages[0][0] == 2 and pages[-1][0] == 4 and all(p[0] == 0 for p in pages[1:-1]), "begin and end flags are wrong")
    head = pages[0][4]
    need(len(head) == 1 and head[0][:8] == b"OpusHead" and len(head[0]) == 19, "first page is not one OpusHead")
    version, ch, skip, rate, gain, family = struct.unpack("<BBHIhB", head[0][8:])
    need((version, ch, rate, gain, family) == (1, channels, 48000, 0, 0), "OpusHead fields are wrong")
    need(len(pages[1][4]) == 1 and pages[1][4][0][:8] == b"OpusTags" and pages[0][1] == 0 and pages[1][1] == 0, "second page is not OpusTags")
    done = 0
    for flags, granule, serial, sequence, packets in pages[2:]:
        span = sum(opus_samples(p) for p in packets)
        need(0 < span <= 48000 + 960, "a page holds more than about a second")
        done += span
        if flags == 4:
            need(granule == skip + frames, f"last granule {granule} is not pre-skip {skip} + {frames}")
            need(done - span < granule <= done, "last granule is outside the last page")
        else:
            need(granule == done, "page granule is not the sample count so far")
    print(f"  ogg {os.path.basename(path)}: {len(pages)} pages, pre-skip {skip}, end granule {pages[-1][1]}")


def read_fixture(path):
    data = open(path, "rb").read()
    need(data[:4] == b"APKT", "not a packet file")
    count = struct.unpack(">I", data[4:8])[0]
    at = 8
    tracks = []
    for _ in range(count):
        fields = struct.unpack(">B4s4sIHHIHBBqqBBBBI", data[at:at + 49])
        at += 49
        track = dict(zip(["audio", "codec", "config_type", "timescale", "width", "height", "rate", "channels", "bits", "flags", "skip", "length"], fields[:12]))
        track["config"] = data[at:at + fields[16]]
        at += fields[16]
        packets = []
        number = struct.unpack(">I", data[at:at + 4])[0]
        at += 4
        for _ in range(number):
            pts, dts, duration, key, size = struct.unpack(">qqIBI", data[at:at + 25])
            packets.append((pts, dts, duration, key, data[at + 25:at + 25 + size]))
            at += 25 + size
        track["packets"] = packets
        tracks.append(track)
    return tracks


def fixtures(paths):
    return [track for path in paths for track in read_fixture(path)]


def boxes(data, start, end):
    at = start
    while at + 8 <= end:
        size, kind = struct.unpack(">I4s", data[at:at + 8])
        head = 8
        if size == 1:
            size = struct.unpack(">Q", data[at + 8:at + 16])[0]
            head = 16
        need(size >= head and at + size <= end, f"box {kind} has a bad size")
        yield kind, at + head, at + size
        at += size
    need(at == end, "boxes do not fill their parent")


def find(data, start, end, *path):
    for kind, a, b in boxes(data, start, end):
        if kind == path[0]:
            return (a, b) if len(path) == 1 else find(data, a, b, *path[1:])
    return None


def mp4_entry(track, brand):
    codec = track["codec"]
    quick = brand == b"qt  "
    if codec == b"lpcm":
        floating, big, bits = track["flags"] & 1, track["flags"] & 2, track["bits"]
        if not quick:
            return (b"fpcm" if floating else b"ipcm"), [b"pcmC"]
        if not floating and bits == 16 and track["channels"] <= 2:
            return (b"twos" if big else b"sowt"), []
        if not floating and bits == 24 and track["channels"] <= 2:
            return b"in24", [b"wave"]
        return b"lpcm", []
    inner = {b"avc1": b"avcC", b"hvc1": b"hvcC", b"av01": b"av1C", b"vp09": b"vpcC", b"vp08": b"vpcC", b"mp4a": b"esds", b"Opus": b"dOps", b"fLaC": b"dfLa", b"alac": b"alac"}.get(codec)
    if quick and codec in (b"mp4a", b"alac"):
        return codec, [b"wave"]
    return codec, [inner] if inner else []


def mp4(path, brand, sources):
    data = open(path, "rb").read()
    wanted = fixtures(sources)
    brand = brand.encode().ljust(4)
    top = {kind: (a, b) for kind, a, b in boxes(data, 0, len(data))}
    need(list(top) == [b"ftyp", b"moov", b"mdat"], "top-level boxes are not ftyp, moov, mdat")
    need(data[top[b"ftyp"][0]:top[b"ftyp"][0] + 4] == brand, "major brand is wrong")
    traks = [(a, b) for kind, a, b in boxes(data, *top[b"moov"]) if kind == b"trak"]
    need(len(traks) == len(wanted), "track count differs")
    for (a, b), track in zip(traks, wanted):
        stbl = find(data, a, b, b"mdia", b"minf", b"stbl")
        table = {kind: (x, y) for kind, x, y in boxes(data, *stbl)}
        stsd = table[b"stsd"][0]
        entry_size, entry = struct.unpack(">I4s", data[stsd + 8:stsd + 16])
        name, inner = mp4_entry(track, brand)
        need(entry == name, f"sample entry is {entry}, expected {name}")
        body = data[stsd + 8:stsd + 8 + entry_size]
        for kind in inner + ([] if track["audio"] else [b"colr"]):
            need(kind in body[16:], f"{name} entry has no {kind} box")
        if not track["audio"]:
            need((b"nclc" if brand == b"qt  " else b"nclx") in body, "colour box has the wrong kind for this file type")
            need(struct.unpack(">HH", body[32:36]) == (track["width"], track["height"]), "picture size is wrong")
        x = table[b"stsz"][0]
        fixed_size, count = struct.unpack(">II", data[x + 4:x + 12])
        sizes = [fixed_size] * count if fixed_size else list(struct.unpack(f">{count}I", data[x + 12:x + 12 + 4 * count]))
        if b"co64" in table:
            x = table[b"co64"][0]
            n = struct.unpack(">I", data[x + 4:x + 8])[0]
            chunks = struct.unpack(f">{n}Q", data[x + 8:x + 8 + 8 * n])
        else:
            x = table[b"stco"][0]
            n = struct.unpack(">I", data[x + 4:x + 8])[0]
            chunks = struct.unpack(f">{n}I", data[x + 8:x + 8 + 4 * n])
        x = table[b"stsc"][0]
        n = struct.unpack(">I", data[x + 4:x + 8])[0]
        runs = [struct.unpack(">III", data[x + 8 + 12 * i:x + 20 + 12 * i]) for i in range(n)]
        samples = []
        index = 0
        for r, (first, per, _) in enumerate(runs):
            last = runs[r + 1][0] if r + 1 < len(runs) else len(chunks) + 1
            for c in range(first, last):
                offset = chunks[c - 1]
                for _ in range(per):
                    need(index < count, "chunk table holds more samples than the size table")
                    samples.append(data[offset:offset + sizes[index]])
                    need(top[b"mdat"][0] <= offset and offset + sizes[index] <= top[b"mdat"][1], "a sample lies outside mdat")
                    offset += sizes[index]
                    index += 1
        need(index == count, "chunk table holds fewer samples than the size table")
        x = table[b"stts"][0]
        n = struct.unpack(">I", data[x + 4:x + 8])[0]
        deltas = []
        for i in range(n):
            c, d = struct.unpack(">II", data[x + 8 + 8 * i:x + 16 + 8 * i])
            deltas += [d] * c
        need(len(deltas) == count, "time table and size table disagree")
        payload = b"".join(p[4] for p in track["packets"])
        if track["codec"] == b"lpcm":
            need(b"".join(samples) == payload, "PCM bytes differ")
            need(fixed_size == track["channels"] * track["bits"] // 8 and set(deltas) == {1}, "PCM sample size or timing is wrong")
        else:
            need(samples == [p[4] for p in track["packets"]], "packets differ")
            order = sorted(track["packets"], key=lambda p: p[1])
            need(deltas[:-1] == [b[1] - a[1] for a, b in zip(order, order[1:])], "decode time steps differ")
            keys = [i + 1 for i, p in enumerate(track["packets"]) if p[3]]
            if b"stss" in table:
                x = table[b"stss"][0]
                n = struct.unpack(">I", data[x + 4:x + 8])[0]
                need(list(struct.unpack(f">{n}I", data[x + 8:x + 8 + 4 * n])) == keys, "key frame table differs")
            else:
                need(len(keys) == count, "key frame table is missing")
            offsets = [0] * count
            if b"ctts" in table:
                x = table[b"ctts"][0]
                n = struct.unpack(">I", data[x + 4:x + 8])[0]
                offsets = []
                for i in range(n):
                    c, d = struct.unpack(">Ii", data[x + 8 + 8 * i:x + 16 + 8 * i])
                    offsets += [d] * c
            shift = min(0, min(p[0] - p[1] for p in track["packets"]))
            need(offsets == [p[0] - p[1] - shift for p in track["packets"]], "presentation offsets differ")
    print(f"  mp4 {os.path.basename(path)}: {len(traks)} track(s), every packet in place")


def vint(data, at, keep):
    first = data[at]
    need(first != 0, "bad EBML number")
    length = 1
    while not first & (0x80 >> (length - 1)):
        length += 1
    value = first if keep else first & (0xFF >> length)
    for i in range(1, length):
        value = (value << 8) | data[at + i]
    return value, at + length


def elements(data, start, end):
    at = start
    while at < end:
        ident, at = vint(data, at, True)
        size, at = vint(data, at, False)
        need(at + size <= end, f"element {ident:X} runs past its parent")
        yield ident, at, at + size
        at += size
    need(at == end, "elements do not fill their parent")


def number(data, a, b):
    return int.from_bytes(data[a:b], "big")


MKV_NAMES = {b"avc1": "V_MPEG4/ISO/AVC", b"hvc1": "V_MPEGH/ISO/HEVC", b"av01": "V_AV1", b"vp09": "V_VP9", b"vp08": "V_VP8", b"mp4a": "A_AAC", b"Opus": "A_OPUS", b"fLaC": "A_FLAC",
             b"alac": "A_ALAC"}


def mkv(path, doctype, sources):
    data = open(path, "rb").read()
    wanted = fixtures(sources)
    top = list(elements(data, 0, len(data)))
    need([t[0] for t in top] == [0x1A45DFA3, 0x18538067], "file is not one EBML header and one Segment")
    header = {i: (a, b) for i, a, b in elements(data, top[0][1], top[0][2])}
    need(data[header[0x4282][0]:header[0x4282][1]].decode() == doctype, "DocType is wrong")
    base = top[1][1]
    parts = list(elements(data, base, top[1][2]))
    need([p[0] for p in parts[:4]] == [0x114D9B74, 0x1549A966, 0x1654AE6B, 0x1C53BB6B], "Segment does not start with SeekHead, Info, Tracks, Cues")
    for ident, a, b in elements(data, parts[0][1], parts[0][2]):
        seek = {i: (x, y) for i, x, y in elements(data, a, b)}
        target = number(data, *seek[0x53AB])
        position = base + number(data, *seek[0x53AC])
        need(vint(data, position, True)[0] == target, f"SeekHead entry for {target:X} points elsewhere")
    entries = [(a, b) for i, a, b in elements(data, parts[2][1], parts[2][2]) if i == 0xAE]
    need(len(entries) == len(wanted), "track count differs")
    for (a, b), track in zip(entries, wanted):
        fields = {i: (x, y) for i, x, y in elements(data, a, b)}
        name = data[fields[0x86][0]:fields[0x86][1]].decode()
        secret = data[fields[0x63A2][0]:fields[0x63A2][1]] if 0x63A2 in fields else b""
        if track["codec"] == b"lpcm":
            want = "A_PCM/FLOAT/IEEE" if track["flags"] & 1 else "A_PCM/INT/BIG" if track["flags"] & 2 else "A_PCM/INT/LIT"
        else:
            want = MKV_NAMES[track["codec"]]
        need(name == want, f"CodecID is {name}, expected {want}")
        if track["codec"] == b"fLaC":
            need(secret == b"fLaC\x80\0\0\x22" + track["config"], "FLAC CodecPrivate is wrong")
        elif track["codec"] in (b"vp09", b"vp08", b"lpcm"):
            need(secret == b"", "CodecPrivate should be absent")
        elif track["config_type"] != b"esds":
            need(secret == track["config"], "CodecPrivate differs from the codec configuration")
        if track["audio"]:
            audio = {i: (x, y) for i, x, y in elements(data, *fields[0xE1])}
            need(struct.unpack(">d", data[audio[0xB5][0]:audio[0xB5][1]])[0] == track["rate"] and number(data, *audio[0x9F]) == track["channels"], "audio fields are wrong")
            if track["codec"] == b"lpcm":
                need(number(data, *audio[0x6264]) == track["bits"], "BitDepth is wrong")
        else:
            video = {i: (x, y) for i, x, y in elements(data, *fields[0xE0])}
            need((number(data, *video[0xB0]), number(data, *video[0xBA])) == (track["width"], track["height"]), "picture size is wrong")
            colour = {i: number(data, x, y) for i, x, y in elements(data, *video[0x55B0])}
            need(colour == {0x55B1: 1, 0x55B9: 1, 0x55BA: 1, 0x55BB: 1}, "Colour element is wrong")
    got = [[] for _ in wanted]
    clusters = {}
    for ident, a, b in parts[4:]:
        need(ident == 0x1F43B675, "something other than Clusters follows the Cues")
        time = None
        for kind, x, y in elements(data, a, b):
            if kind == 0xE7:
                time = number(data, x, y)
                clusters[a] = time
            blocks = [(x, y, True)] if kind == 0xA3 else [(p, q, False) for k, p, q in elements(data, x, y) if k == 0xA1] if kind == 0xA0 else []
            for p, q, simple in blocks:
                track, at = vint(data, p, False)
                relative = struct.unpack(">h", data[at:at + 2])[0]
                key = bool(data[at + 2] & 0x80) if simple else True
                got[track - 1].append((time + relative, key, data[at + 3:q]))
    for track, blocks in zip(wanted, got):
        packets = sorted(track["packets"], key=lambda p: p[0]) if not track["audio"] else track["packets"]
        blocks = sorted(blocks, key=lambda p: p[0]) if not track["audio"] else blocks
        need([b[2] for b in blocks] == [p[4] for p in packets], "packets differ")
        need([b[0] for b in blocks] == [int(numpy.floor(p[0] * 1000 / track["timescale"] + 0.5)) for p in packets], "block times differ")
        if not track["audio"]:
            need([b[1] for b in blocks] == [bool(p[3]) for p in packets], "key frame flags differ")
    points = 0
    for ident, a, b in elements(data, parts[3][1], parts[3][2]):
        point = {i: (x, y) for i, x, y in elements(data, a, b)}
        where = {i: number(data, x, y) for i, x, y in elements(data, *point[0xB7])}
        position = base + where[0xF1]
        ident_at, body = vint(data, position, True)
        need(ident_at == 0x1F43B675, "a cue does not point at a Cluster")
        body = vint(data, body, False)[1]
        need(clusters.get(body) == number(data, *point[0xB3]), "a cue time is not its Cluster time")
        points += 1
    need(points > 0, "no cues")
    print(f"  mkv {os.path.basename(path)}: {len(entries)} track(s), every packet in place, {points} cue(s)")


def copied(path, sources):
    wanted = fixtures(sources)
    for index, track in enumerate(wanted):
        got = subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-i", path, "-map", f"0:{index}", "-c", "copy", "-f", "data", "-"], capture_output=True).stdout
        need(got == b"".join(p[4] for p in track["packets"]), f"FFmpeg reads different bytes from track {index}")
        if track["codec"] == b"lpcm":
            name = subprocess.run([FFPROBE, "-v", "error", "-select_streams", str(index), "-show_entries", "stream=codec_name", "-of", "csv=p=0", path], capture_output=True).stdout.decode().strip()
            kind = ("f" if track["flags"] & 1 else "s") + str(track["bits"]) + ("be" if track["flags"] & 2 else "le")
            need(name == "pcm_" + kind, f"FFmpeg sees {name}, expected pcm_{kind}")
    print(f"  ffmpeg reads the same packets from {os.path.basename(path)}")


def payload(source, swap):
    track = read_fixture(source)[0]
    raw = b"".join(p[4] for p in track["packets"])
    if swap:
        raw = numpy.frombuffer(raw, dtype=">i2").astype("<i2").tobytes()
    sys.stdout.buffer.write(raw)


def lossy(decoded, source, channels):
    got = numpy.fromfile(decoded, dtype="<i2").astype(numpy.float64)
    want = numpy.frombuffer(expected(source, "16"), dtype="<i2").astype(numpy.float64)
    need(len(got) == len(want), f"decoded {len(got) // channels} frames, expected {len(want) // channels}")
    edge = 2400 * channels
    a, b = got[edge:-edge], want[edge:-edge]
    noise = numpy.mean((a - b) ** 2)
    snr = 99.0 if noise == 0 else 10 * numpy.log10(numpy.mean(b ** 2) / noise)
    need(snr > 20, f"SNR is only {snr:.1f} dB")
    print(f"  {os.path.basename(decoded)}: {len(got) // channels} frames, SNR {snr:.1f} dB")


def flac_audio(path):
    data = open(path, "rb").read()
    at = 4
    while True:
        last = data[at] >> 7
        at += 4 + int.from_bytes(data[at + 1:at + 4], "big")
        if last:
            break
    print(len(data) - at)


def same(decoded, source, kind):
    got = open(decoded, "rb").read()
    want = expected(source, kind)
    need(len(got) == len(want), f"decoded {len(got)} bytes, expected {len(want)}")
    need(got == want, "decoded samples differ")


if __name__ == "__main__":
    a = sys.argv[2:]
    kind = sys.argv[1]
    if kind == "signal":
        signal(a[0], a[1], int(a[2]), int(a[3]), int(a[4]))
    elif kind == "wav":
        wav(a[0], a[1], int(a[2]), int(a[3]), a[4], len(a) > 5)
    elif kind == "flac":
        flac(a[0], a[1], int(a[2]), int(a[3]), int(a[4]))
    elif kind == "ogg":
        ogg(a[0], int(a[1]), int(a[2]))
    elif kind == "mp4":
        mp4(a[0], a[1], a[2:])
    elif kind == "mkv":
        mkv(a[0], a[1], a[2:])
    elif kind == "copied":
        copied(a[0], a[1:])
    elif kind == "payload":
        payload(a[0], len(a) > 1)
    elif kind == "lossy":
        lossy(a[0], a[1], int(a[2]))
    elif kind == "flacaudio":
        flac_audio(a[0])
    elif kind == "same":
        same(a[0], a[1], a[2])
    else:
        sys.exit(2)
