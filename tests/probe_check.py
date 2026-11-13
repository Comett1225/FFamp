import json
import os
import random
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.environ.get("AMEDIA", os.path.join(HERE, "..", "build", "amedia"))
FFPROBE = os.environ.get("FFPROBE", "ffprobe")

STREAM_FIELDS = ["codec_name", "codec_type", "profile", "width", "height", "pix_fmt", "color_range", "color_space", "color_transfer",
                 "color_primaries", "sample_fmt", "sample_rate", "channels", "channel_layout", "r_frame_rate", "avg_frame_rate", "time_base",
                 "start_time", "duration", "bit_rate", "nb_frames"]
FORMAT_FIELDS = ["nb_streams", "format_name", "start_time", "duration", "size", "bit_rate"]
TAGS = ["title", "artist", "album", "date", "creation_time"]
CLOSE = {"start_time": 0.0005, "duration": 0.0005}
RATIO = {"bit_rate": 0.02, "r_frame_rate": 0.0001, "avg_frame_rate": 0.0001}


def ours(path, packets=False):
    run = subprocess.run([AMEDIA, "probe", "--json"] + (["--packets"] if packets else []) + [path], capture_output=True, text=True)
    return json.loads(run.stdout) if run.returncode == 0 else None


def theirs(path, packets=False):
    run = subprocess.run([FFPROBE, "-v", "error", "-show_streams", "-show_format"] + (["-show_packets"] if packets else []) + ["-of", "json", path],
                         capture_output=True, text=True)
    return json.loads(run.stdout)


def number(text):
    text = str(text)
    if "/" in text:
        top, bottom = text.split("/")
        return float(top) / float(bottom) if float(bottom) else 0.0
    return float(text)


def same(field, a, b):
    if a == b:
        return True
    try:
        x, y = number(a), number(b)
    except ValueError:
        return False
    if field in CLOSE:
        return abs(x - y) <= CLOSE[field]
    if field in RATIO:
        return abs(x - y) <= RATIO[field] * max(abs(x), abs(y))
    return False


def near(allow, name, a, b):
    if name in allow or name.split(".")[-1] in allow:
        return True
    for item in allow:
        if item.startswith(name + "~") and a is not None:
            return abs(number(a) - number(b)) <= float(item.split("~")[1]) * max(1.0, abs(number(b)) if "rate" in name else 1.0)
    return False


def rotation(stream):
    for side in stream.get("side_data_list", []):
        if "rotation" in side:
            return int(side["rotation"])
    return 0


def compare(path, allow):
    a, b = ours(path), theirs(path)
    if a is None:
        print(f"probe {os.path.basename(path)}: amedia cannot open it")
        return False
    wrong, extra, checked = [], [], 0
    kept = [s for s in b.get("streams", []) if s.get("codec_type") in ("video", "audio")]
    mine = [s for s in a["streams"] if s.get("codec_type") in ("video", "audio")]
    if len(mine) != len(kept):
        wrong.append(f"stream count {len(mine)} against {len(kept)}")
    for index, (x, y) in enumerate(zip(mine, kept)):
        for field in STREAM_FIELDS:
            name = f"stream{index}.{field}"
            if field not in y:
                if field in x:
                    extra.append(name)
                continue
            checked += 1
            if not same(field, x.get(field), y[field]) and not near(allow, name, x.get(field), y[field]):
                wrong.append(f"{name}: ours {x.get(field)!r}, ffprobe {y[field]!r}")
        checked += 2
        if rotation(x) != rotation(y):
            wrong.append(f"stream{index}.rotation: ours {rotation(x)}, ffprobe {rotation(y)}")
        want = y.get("tags", {}).get("language")
        have = x.get("tags", {}).get("language")
        if want != have and "language" not in allow:
            wrong.append(f"stream{index}.language: ours {have!r}, ffprobe {want!r}")
    for field in FORMAT_FIELDS:
        checked += 1
        if not same(field, a["format"].get(field), b["format"].get(field)) and not near(allow, "format." + field, a["format"].get(field), b["format"].get(field)):
            wrong.append(f"format.{field}: ours {a['format'].get(field)!r}, ffprobe {b['format'].get(field)!r}")
    for tag in TAGS:
        want = {k.lower(): v for k, v in b["format"].get("tags", {}).items()}.get(tag)
        have = a["format"].get("tags", {}).get(tag)
        checked += 1
        if want != have and "tag." + tag not in allow:
            wrong.append(f"format.tags.{tag}: ours {have!r}, ffprobe {want!r}")
    print(f"probe {os.path.basename(path)}: {checked} fields checked, {len(wrong)} differ, {len(extra)} only in ours")
    for line in wrong:
        print("   " + line)
    return not wrong


def packets(path, allow):
    a, b = ours(path, True), theirs(path, True)
    if a is None:
        print(f"packets {os.path.basename(path)}: amedia cannot open it")
        return False
    kept = [s["index"] for s in b.get("streams", []) if s.get("codec_type") in ("video", "audio")]
    good = True
    for index, their_index in enumerate(kept):
        mine = [p for p in a["packets"] if p["stream_index"] == index]
        yours = [p for p in b["packets"] if p["stream_index"] == their_index]
        fields = [f for f in ("pts", "dts", "duration", "flags", "size", "pos") if f"stream{index}.{f}" not in allow and f not in allow]
        bad = 0
        first = ""
        for x, y in zip(mine, yours):
            for field in fields:
                want = y.get(field)
                have = x.get(field)
                if field == "flags":
                    want, have = want[:1], have[:1]
                if field == "size" and f"stream{index}.size+7" in allow:
                    have = str(int(have) + 7)
                slack = [int(item.split("~")[1]) for item in allow if item.startswith(f"stream{index}.{field}~")]
                if slack and want is not None and abs(int(want) - int(have)) <= slack[0]:
                    continue
                if str(want) != str(have):
                    bad += 1
                    first = first or f"{field}: ours {have}, ffprobe {want} at packet {mine.index(x)}"
        if len(mine) != len(yours):
            bad += 1
            first = first or f"count {len(mine)} against {len(yours)}"
        print(f"packets {os.path.basename(path)} stream {index}: {len(mine)} packets, fields {','.join(fields)}, {bad} differences {first}")
        good = good and bad == 0
    return good


def stripped(path, with_packets):
    data = ours(path, with_packets)
    if data is not None:
        data["format"].pop("filename", None)
    return data


def expect(path, expected, with_packets):
    data = stripped(path, with_packets)
    with open(expected) as f:
        want = json.load(f)
    if data == want:
        return True
    print(f"expect {os.path.basename(path)}: output differs from {os.path.basename(expected)}")
    return False


def write(path, expected, with_packets):
    data = stripped(path, with_packets)
    if data is None:
        return False
    with open(expected, "w") as f:
        json.dump(data, f, separators=(",", ":"))
        f.write("\n")
    return True


def boxes(data, start, end):
    at = start
    while at + 8 <= end:
        size, kind = struct.unpack(">I4s", data[at:at + 8])
        if size < 8 or at + size > end:
            break
        yield kind, at, size
        at += size


def find(data, start, end, kind):
    for name, at, size in boxes(data, start, end):
        if name == kind:
            return at, size
    return None


def edits(source, target):
    data = bytearray(open(source, "rb").read())
    moov = find(data, 0, len(data), b"moov")
    assert moov and moov[0] + moov[1] == len(data)
    trak = find(data, moov[0] + 8, moov[0] + moov[1], b"trak")
    edts = find(data, trak[0] + 8, trak[0] + trak[1], b"edts")
    elst = find(data, edts[0] + 8, edts[0] + edts[1], b"elst")
    mvhd = find(data, moov[0] + 8, moov[0] + moov[1], b"mvhd")
    scale = struct.unpack(">I", data[mvhd[0] + 20:mvhd[0] + 24])[0]
    entries = [(scale // 4, -1), (scale, 1024), (scale // 2, 24064)]
    body = struct.pack(">II", 0, len(entries)) + b"".join(struct.pack(">IiI", d, t, 0x10000) for d, t in entries)
    fresh = struct.pack(">I4s", 8 + len(body), b"elst") + body
    grow = len(fresh) - elst[1]
    data[elst[0]:elst[0] + elst[1]] = fresh
    for at, size in (edts, trak, moov):
        data[at:at + 4] = struct.pack(">I", size + grow)
    open(target, "wb").write(data)
    return True


def survives(path, extra):
    run = subprocess.run([AMEDIA, "probe", "--json", "--packets", path], capture_output=True)
    if run.returncode not in (0, 1):
        return False
    if run.returncode == 0:
        json.loads(run.stdout.decode("utf-8"))
        run = subprocess.run([AMEDIA, extra, path], capture_output=True)
        if run.returncode not in (0, 1):
            return False
    return True


def damage(path, folder, rounds, seed):
    original = open(path, "rb").read()
    rng = random.Random(seed)
    name = os.path.join(folder, "damaged_" + os.path.basename(path))
    crashes = 0
    opened = 0
    for n in range(rounds):
        data = bytearray(original)
        kind = n % 5
        if kind == 0:
            data = data[:rng.randrange(1, len(data))]
        elif kind == 1:
            for _ in range(rng.randrange(1, 40)):
                data[rng.randrange(len(data))] = rng.randrange(256)
        elif kind == 2:
            for _ in range(rng.randrange(1, 12)):
                at = rng.randrange(len(data))
                del data[at:at + rng.randrange(1, 600)]
        elif kind == 3:
            at = rng.randrange(len(data))
            data[at:at + rng.randrange(1, 3000)] = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 3000)))
        else:
            at = rng.randrange(max(1, len(data) // 4))
            data = data[:1024] + data[1024 + at:]
        with open(name, "wb") as f:
            f.write(data)
        if not survives(name, "decode"):
            crashes += 1
            with open(os.path.join(folder, f"crash_{n}_" + os.path.basename(path)), "wb") as f:
                f.write(data)
        elif ours(name) is not None:
            opened += 1
    os.remove(name)
    print(f"damage {os.path.basename(path)}: {rounds} damaged copies, {opened} still open, {crashes} crashes")
    return crashes == 0


def main():
    mode = sys.argv[1]
    if mode == "compare":
        return compare(sys.argv[2], set(sys.argv[3:]))
    if mode == "packets":
        return packets(sys.argv[2], set(sys.argv[3:]))
    if mode == "expect":
        return expect(sys.argv[2], sys.argv[3], len(sys.argv) > 4)
    if mode == "write":
        return write(sys.argv[2], sys.argv[3], len(sys.argv) > 4)
    if mode == "edits":
        return edits(sys.argv[2], sys.argv[3])
    if mode == "damage":
        return damage(sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]))
    return False


if __name__ == "__main__":
    sys.exit(0 if main() else 1)
