import struct
import subprocess
import sys


def probe_packets(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "0", "-show_entries", "packet=pos,size", "-of", "csv=p=0", path],
                         capture_output=True, text=True, check=True).stdout.split()
    return [tuple(int(x) for x in reversed(line.split(",")[:2])) for line in out]


def crc8(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def coded_number(value):
    if value < 0x80:
        return bytes([value])
    count = 2
    while value >= 1 << (5 * count + 1):
        count += 1
    lead = (0xFF << (8 - count)) & 0xFF | (value >> (6 * (count - 1)))
    return bytes([lead] + [0x80 | ((value >> (6 * i)) & 0x3F) for i in range(count - 2, -1, -1)])


def coded_length(lead):
    length = 0
    while lead & (0x80 >> length):
        length += 1
    return max(length, 1)


def flac_variable(source, target):
    data = open(source, "rb").read()
    packets = probe_packets(source)
    block = struct.unpack(">H", data[8:10])[0]
    out = bytearray(data[:packets[0][0]])
    out[8:10] = struct.pack(">H", 16)
    for index, (pos, size) in enumerate(packets):
        frame = data[pos:pos + size]
        skip = coded_length(frame[4])
        rest = frame[4 + skip:]
        tail = (1 if frame[2] >> 4 == 6 else 2 if frame[2] >> 4 == 7 else 0) + (1 if frame[2] & 15 == 12 else 2 if frame[2] & 15 in (13, 14) else 0)
        head = bytes([0xFF, 0xF9, frame[2], frame[3]]) + coded_number(index * block) + rest[:tail]
        body = head + bytes([crc8(head)]) + rest[tail + 1:-2]
        out += body + struct.pack(">H", crc16(body))
    open(target, "wb").write(out)


def adts_checksum(source, target):
    data = open(source, "rb").read()
    out = bytearray()
    at = 0
    while at + 7 <= len(data):
        size = ((data[at + 3] & 3) << 11) | (data[at + 4] << 3) | (data[at + 5] >> 5)
        size += 2
        head = bytearray(data[at:at + 7])
        head[1] &= 0xFE
        head[3] = (head[3] & 0xFC) | (size >> 11)
        head[4] = (size >> 3) & 0xFF
        head[5] = (head[5] & 0x1F) | ((size & 7) << 5)
        out += head + b"\x12\x34" + data[at + 7:at + size - 2]
        at += size - 2
    open(target, "wb").write(out)


def ogg_crc(page):
    crc = 0
    for byte in page:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def ogg_small_pages(source, target, limit):
    data = open(source, "rb").read()
    at = 0
    packets = []
    current = bytearray()
    serial = None
    while at + 27 <= len(data):
        count = data[at + 26]
        table = data[at + 27:at + 27 + count]
        granule = struct.unpack("<q", data[at + 6:at + 14])[0]
        serial = data[at + 14:at + 18]
        body = at + 27 + count
        done = []
        for lace in table:
            current += data[body:body + lace]
            body += lace
            if lace < 255:
                done.append(bytes(current))
                current = bytearray()
        for i, packet in enumerate(done):
            packets.append((packet, granule if i == len(done) - 1 else -1))
        at = body
    out = bytearray()
    number = 0
    position = 0
    opus = packets[0][0].startswith(b"OpusHead")
    block = 0 if opus else struct.unpack(">H", packets[0][0][17:19])[0]
    for index, (packet, granule) in enumerate(packets):
        if opus and index >= 2:
            config = packet[0] >> 3
            frame = [480, 960, 1920, 2880][config & 3] if config < 12 else [480, 960][config & 1] if config < 16 else 120 << (config & 3)
            position += frame * (1 if packet[0] & 3 == 0 else 2 if packet[0] & 3 < 3 else packet[1] & 63)
        elif not opus and packet[0] == 0xFF:
            position += block
        if index == len(packets) - 1:
            position = granule
        laces = [255] * (len(packet) // 255) + [len(packet) % 255]
        offset = 0
        first = True
        while laces:
            part = laces[:limit]
            laces = laces[limit:]
            size = sum(part)
            flags = (0 if first else 1) | (2 if index == 0 and first else 0) | (4 if index == len(packets) - 1 and not laces else 0)
            page = bytearray(b"OggS\x00" + bytes([flags]) + struct.pack("<q", position if not laces else -1) + serial + struct.pack("<I", number) + b"\x00\x00\x00\x00" +
                             bytes([len(part)]) + bytes(part) + packet[offset:offset + size])
            page[22:26] = struct.pack("<I", ogg_crc(page))
            out += page
            offset += size
            number += 1
            first = False
    open(target, "wb").write(out)


def ebml_id(value):
    return value.to_bytes((value.bit_length() + 7) // 8, "big")


def ebml_size(value):
    return (value | (1 << 56)).to_bytes(8, "big")


def el(ident, body):
    return ebml_id(ident) + ebml_size(len(body)) + body


def uint(ident, value):
    return el(ident, value.to_bytes(max(1, (value.bit_length() + 7) // 8), "big"))


def xiph_sizes(sizes):
    out = bytearray()
    for size in sizes:
        out += b"\xff" * (size // 255) + bytes([size % 255])
    return bytes(out)


def ebml_number(value, length):
    return (value | (1 << (7 * length))).to_bytes(length, "big")


def ebml_sizes(sizes):
    out = bytearray(ebml_number(sizes[0], 2))
    for previous, size in zip(sizes, sizes[1:]):
        out += ebml_number(size - previous + (1 << 13) - 1, 2)
    return bytes(out)


def mp3_frames(path):
    data = open(path, "rb").read()
    return [data[pos:pos + size] for pos, size in probe_packets(path)]


def laced_mkv(source, target, mode, strip, rate):
    frames = mp3_frames(source)
    header = el(0x1A45DFA3, uint(0x4286, 1) + uint(0x42F7, 1) + uint(0x42F2, 4) + uint(0x42F3, 8) + el(0x4282, b"matroska") + uint(0x4287, 4) + uint(0x4285, 2))
    info = el(0x1549A966, uint(0x2AD7B1, 1000000) + el(0x4D80, b"craft") + el(0x5741, b"craft") + el(0x7BA9, b"Laced"))
    encodings = b""
    if strip:
        prefix = frames[0][:strip]
        assert all(f[:strip] == prefix for f in frames)
        frames = [f[strip:] for f in frames]
        encodings = el(0x6D80, el(0x6240, el(0x5034, uint(0x4254, 3) + el(0x4255, prefix))))
    audio = el(0xE1, el(0xB5, struct.pack(">d", float(rate))) + uint(0x9F, 2))
    track = el(0xAE, uint(0xD7, 1) + uint(0x73C5, 7) + uint(0x83, 2) + uint(0x9C, 1) + el(0x86, b"A_MPEG/L3") + audio + encodings)
    tracks = el(0x1654AE6B, track)
    clusters = b""
    per_block = 4
    per_cluster = 5
    blocks = []
    for start in range(0, len(frames), per_block):
        group = frames[start:start + per_block]
        time = round(start * 1152 * 1000 / rate)
        sizes = [len(f) for f in group]
        if mode == "fixed":
            assert len(set(sizes)) == 1
            lacing = bytes([len(group) - 1])
            flags = 0x80 | 0x04
        elif mode == "xiph":
            lacing = bytes([len(group) - 1]) + xiph_sizes(sizes[:-1])
            flags = 0x80 | 0x02
        else:
            lacing = bytes([len(group) - 1]) + (ebml_sizes(sizes[:-1]) if len(group) > 1 else b"")
            flags = 0x80 | 0x06
        if len(group) == 1:
            lacing = b""
            flags = 0x80
        blocks.append((time, flags, lacing + b"".join(group)))
    for start in range(0, len(blocks), per_cluster):
        base = blocks[start][0]
        body = uint(0xE7, base)
        for i, (time, flags, payload) in enumerate(blocks[start:start + per_cluster]):
            block = b"\x81" + struct.pack(">h", time - base) + bytes([flags]) + payload
            if i % 2:
                body += el(0xA0, el(0xA1, b"\x81" + struct.pack(">h", time - base) + bytes([flags & 0x7F]) + payload))
            else:
                body += el(0xA3, block)
        clusters += el(0x1F43B675, body)
    tags = el(0x1254C367, el(0x7373, el(0x63C0, b"") + el(0x67C8, el(0x45A3, b"ARTIST") + el(0x4487, b"Craft Artist"))))
    segment = info + tracks + clusters + tags
    open(target, "wb").write(header + ebml_id(0x18538067) + ebml_size(len(segment)) + segment)


def vbri(source, target):
    data = open(source, "rb").read()
    size = probe_packets(source)[0][1]
    frame = bytearray(data[:4] + bytes(size - 4))
    frame[36:40] = b"VBRI"
    frame[40:42] = struct.pack(">H", 1)
    open(target, "wb").write(bytes(frame) + data)


if __name__ == "__main__":
    name = sys.argv[1]
    if name == "vbri":
        vbri(sys.argv[2], sys.argv[3])
    if name == "flac_variable":
        flac_variable(sys.argv[2], sys.argv[3])
    elif name == "adts_checksum":
        adts_checksum(sys.argv[2], sys.argv[3])
    elif name == "ogg_small_pages":
        ogg_small_pages(sys.argv[2], sys.argv[3], int(sys.argv[4]))
    elif name == "laced_mkv":
        laced_mkv(sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]), int(sys.argv[6]))
