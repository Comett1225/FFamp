import struct
import sys
import uuid

HEADER = uuid.UUID("75B22630-668E-11CF-A6D9-00AA0062CE6C").bytes_le
DATA = uuid.UUID("75B22636-668E-11CF-A6D9-00AA0062CE6C").bytes_le
FILE = uuid.UUID("8CABDCA1-A947-11CF-8EE4-00C00C205365").bytes_le
STREAM = uuid.UUID("B7DC0791-A9B7-11CF-8EE6-00C00C205365").bytes_le
SIMPLE_INDEX = uuid.UUID("33000890-E5B1-11CF-89F4-00A0C90349CB").bytes_le
AUDIO = uuid.UUID("F8699E40-5B4D-11CF-A8FD-00805F5C442B").bytes_le
SPREAD = uuid.UUID("BFC3CD50-618F-11CF-8BB2-00AA00B4E220").bytes_le
ENCRYPTION = uuid.UUID("2211B3FB-BD23-11D2-B4B7-00A0C955FC6E").bytes_le
UNKNOWN = uuid.UUID("A1B2C3D4-0000-4000-8000-000000000001").bytes_le


def objects(data, start, end):
    at = start
    while at + 24 <= end:
        size = struct.unpack_from("<Q", data, at + 16)[0]
        if size < 24 or at + size > end:
            break
        yield data[at:at + 16], at, size
        at += size


def find(data, guid, start=0, end=None):
    for kind, at, size in objects(data, start, len(data) if end is None else end):
        if kind == guid:
            return at, size
    return None


def sized(data, at, kind):
    width = (0, 1, 2, 4)[kind]
    value = int.from_bytes(data[at:at + width], "little") if width else 0
    return value, at + width


def walk(data):
    header = find(data, HEADER)
    props = find(data, FILE, header[0] + 30, header[0] + header[1])
    packet_size = struct.unpack_from("<I", data, props[0] + 24 + 68)[0]
    body = find(data, DATA)
    at = body[0] + 50
    end = min(body[0] + body[1], len(data))
    packets = []
    while at + packet_size <= end:
        p = at
        flags = data[p]
        p += 1
        if flags & 0x80:
            p += flags & 0x0F
            flags = data[p]
            p += 1
        top = p - 1
        prop = data[p]
        p += 1
        _, p = sized(data, p, (flags >> 5) & 3)
        _, p = sized(data, p, (flags >> 1) & 3)
        padding_at = p
        padding, p = sized(data, p, (flags >> 3) & 3)
        p += 6
        count, length_kind = 1, 2
        if flags & 1:
            count, length_kind = data[p] & 0x3F, data[p] >> 6
            p += 1
        payloads = []
        for _ in range(count):
            stream_at = p
            p += 1
            number, p = sized(data, p, (prop >> 4) & 3)
            offset, p = sized(data, p, (prop >> 2) & 3)
            replicated, p = sized(data, p, prop & 3)
            size = struct.unpack_from("<I", data, p)[0] if replicated >= 8 else 0
            p += replicated
            length, p = sized(data, p, length_kind) if flags & 1 else (at + packet_size - padding - p, p)
            payloads.append({"stream_at": stream_at, "number": number, "offset": offset, "size": size, "at": p, "length": length})
            p += length
        packets.append({"at": at, "flags_at": top, "flags": flags, "padding_at": padding_at, "padding": padding, "payloads": payloads})
        at += packet_size
    return packet_size, packets


def media_objects(data, stream):
    _, packets = walk(data)
    found, current = [], None
    for packet in packets:
        for payload in packet["payloads"]:
            if data[payload["stream_at"]] & 0x7F != stream:
                continue
            if payload["offset"] == 0:
                current = {"size": payload["size"], "pieces": []}
                found.append(current)
            current["pieces"].append((payload["at"], payload["length"]))
    return [o for o in found if sum(n for _, n in o["pieces"]) == o["size"]]


def keyless(data):
    _, packets = walk(data)
    for packet in packets:
        for payload in packet["payloads"]:
            data[payload["stream_at"]] &= 0x7F


def noindex(data):
    while True:
        place = find(data, SIMPLE_INDEX)
        if not place:
            return
        data[place[0]:place[0] + 16] = UNKNOWN


def audio_stream(data):
    header = find(data, HEADER)
    for kind, at, size in objects(data, header[0] + 30, header[0] + header[1]):
        if kind == STREAM and data[at + 24:at + 40] == AUDIO:
            return at
    raise SystemExit("no audio stream")


def spread(data, span, packet, chunk):
    at = audio_stream(data)
    if data[at + 40:at + 56] != SPREAD:
        raise SystemExit("the audio stream has no spread description to change")
    typed = struct.unpack_from("<I", data, at + 64)[0]
    number = struct.unpack_from("<H", data, at + 72)[0] & 0x7F
    struct.pack_into("<BHH", data, at + 78 + typed, span, packet, chunk)
    group, across = span * packet, packet // chunk
    for item in media_objects(data, number):
        logical = b"".join(bytes(data[a:a + n]) for a, n in item["pieces"])
        stored = bytearray(logical)
        for base in range(0, len(logical) - group + 1, group):
            for out in range(span * across):
                place = (out % across) * span + out // across
                stored[base + place * chunk:base + (place + 1) * chunk] = logical[base + out * chunk:base + (out + 1) * chunk]
        done = 0
        for a, n in item["pieces"]:
            data[a:a + n] = stored[done:done + n]
            done += n


def drm_flag(data):
    at = audio_stream(data)
    typed, corrected = struct.unpack_from("<II", data, at + 64)
    flags = struct.unpack_from("<H", data, at + 72)[0]
    struct.pack_into("<H", data, at + 72, flags | 0x8000)


def drm_object(data):
    header = find(data, HEADER)
    body = struct.pack("<I", 0) + struct.pack("<I", 4) + b"none" + struct.pack("<I", 0) + struct.pack("<I", 0)
    extra = ENCRYPTION + struct.pack("<Q", 24 + len(body)) + body
    end = header[0] + header[1]
    data[end:end] = extra
    struct.pack_into("<Q", data, header[0] + 16, header[1] + len(extra))
    count = struct.unpack_from("<I", data, header[0] + 24)[0]
    struct.pack_into("<I", data, header[0] + 24, count + 1)


def explicit(data):
    packet_size, packets = walk(data)
    for packet in reversed(packets):
        flags, at = packet["flags"], packet["at"]
        if (flags >> 5) & 3 or (flags >> 3) & 3 != 1 or packet["padding"] < 2:
            continue
        start, end = packet["padding_at"], at + packet_size
        tail = bytes(data[start + 1:end - 2])
        data[packet["flags_at"]] = flags | 0x40 | 0x08
        data[start:end] = struct.pack("<H", packet_size) + bytes([packet["padding"] - 2]) + tail


def survey(data):
    _, packets = walk(data)
    multiple = sum(1 for p in packets if p["flags"] & 1)
    corrected = sum(1 for p in packets if data[p["at"]] & 0x80)
    print(f"{len(packets)} packets, {multiple} with several payloads, {corrected} with error correction data")


def main():
    action, source = sys.argv[1], sys.argv[2]
    data = bytearray(open(source, "rb").read())
    if action == "survey":
        survey(data)
        return
    target = sys.argv[3]
    if action == "keyless":
        keyless(data)
    elif action == "noindex":
        noindex(data)
    elif action == "plain":
        keyless(data)
        noindex(data)
    elif action == "spread":
        spread(data, int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]))
    elif action == "drm-flag":
        drm_flag(data)
    elif action == "drm-object":
        drm_object(data)
    elif action == "explicit":
        explicit(data)
    else:
        raise SystemExit("unknown action")
    open(target, "wb").write(data)


if __name__ == "__main__":
    main()
