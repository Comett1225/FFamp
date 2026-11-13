import hashlib
import json
import os
import random
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.environ.get("AMEDIA", os.path.join(HERE, "..", "build", "ffamp"))
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
FFPROBE = os.environ.get("FFPROBE", "ffprobe")
CUT = {"lpcm", ".mp3", ".mp2", ".mp1", "ac-3"}


def ours(path):
    run = subprocess.run([AMEDIA, "packets", path], capture_output=True, text=True)
    if run.returncode != 0:
        return None
    tracks = []
    for line in run.stdout.splitlines():
        words = line.split()
        if words[0] == "track":
            tracks.append({"id": int(words[1]), "kind": words[2], "codec": line.split()[3], "timescale": int(words[5]), "packets": []})
        elif words[0].lstrip("-").isdigit() and tracks:
            tracks[-1]["packets"].append((int(words[0]), int(words[1]), int(words[2]), int(words[3]), words[4] == "K"))
    return tracks


def raw(path, track):
    return subprocess.run([AMEDIA, "packets", path, "--raw", "--track", str(track)], capture_output=True).stdout


def theirs(path):
    run = subprocess.run([FFPROBE, "-v", "error", "-show_streams", "-show_packets", "-show_data_hash", "md5", "-of", "json", path], capture_output=True, text=True)
    data = json.loads(run.stdout)
    streams = [s for s in data.get("streams", []) if s.get("codec_type") in ("video", "audio")]
    for s in streams:
        s["packets"] = [p for p in data.get("packets", []) if p["stream_index"] == s["index"]]
    return streams


def their_bytes(path, index):
    return subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-i", path, "-map", f"0:{index}", "-c", "copy", "-f", "data", "-"], capture_output=True).stdout


def lengths(annexb):
    out = bytearray()
    starts = []
    i = 0
    while i + 3 <= len(annexb):
        if annexb[i] == 0 and annexb[i + 1] == 0 and annexb[i + 2] == 1:
            starts.append(i + 3)
            i += 3
        else:
            i += 1
    for k, begin in enumerate(starts):
        end = starts[k + 1] - 3 if k + 1 < len(starts) else len(annexb)
        while end > begin and annexb[end - 1] == 0:
            end -= 1
        nal = annexb[begin:end]
        if nal and (nal[0] & 31) not in (9, 12):
            out += struct.pack(">I", len(nal)) + nal
    return bytes(out)


def stamp(p):
    value = p.get("pts", "N/A")
    return int(value if value != "N/A" else p["dts"])


def packets(path):
    a, b = ours(path), theirs(path)
    if a is None:
        print(f"packets {os.path.basename(path)}: ffamp cannot open it")
        return False
    if len(a) != len(b):
        print(f"packets {os.path.basename(path)}: {len(a)} streams against {len(b)}")
        return False
    good = True
    for mine, yours in zip(a, b):
        name = f"packets {os.path.basename(path)} stream {mine['id']} {yours['codec_name']}"
        data = raw(path, mine["id"])
        if mine["codec"] in CUT:
            want = their_bytes(path, yours["index"])
            first = mine["packets"][0][0] * 1000 / mine["timescale"] if mine["packets"] else 0
            start = stamp(yours["packets"][0]) if yours["packets"] else 0
            ok = data == want and abs(first - start) <= 1
            print(f"{name}: {len(data)} bytes as one stream, {'same' if data == want else 'different'} bytes, first time {first:.1f} ms against {start} ms")
            good = good and ok
            continue
        bad = []
        extra = len(yours["packets"]) - len(mine["packets"])
        if extra and not (cut_short(path) and 0 < extra <= 2):
            bad.append(f"count {len(mine['packets'])} against {len(yours['packets'])}")
        at = 0
        avc = mine["codec"] == "avc1"
        for k, (x, y) in enumerate(zip(mine["packets"], yours["packets"])):
            body = data[at:at + x[3]]
            at += x[3]
            want = y.get("data_hash", "")
            if avc:
                if hashlib.md5(body).hexdigest() != hashlib.md5(lengths(their_packet(path, yours["index"], k))).hexdigest():
                    bad.append(f"bytes of packet {k}")
            elif "MD5:" + hashlib.md5(body).hexdigest() != want:
                bad.append(f"bytes of packet {k}")
            if not avc and x[0] != stamp(y):
                bad.append(f"time of packet {k}: ours {x[0]}, ffprobe {stamp(y)}")
            if mine["kind"] == "video" and x[4] != (y["flags"][0] == "K"):
                bad.append(f"key flag of packet {k}")
        if avc and sorted(x[0] for x in mine["packets"]) != sorted(stamp(y) for y in yours["packets"]):
            bad.append("the times are not the same set")
        print(f"{name}: {len(mine['packets'])} packets, {len(bad)} differences {bad[0] if bad else ''}")
        good = good and not bad
    return good


SPLIT = {}


def cut_short(path):
    data = open(path, "rb").read(1 << 20)
    at = struct.unpack_from("<Q", data, 16)[0]
    return at + 24 <= len(data) and at + struct.unpack_from("<Q", data, at + 16)[0] > os.path.getsize(path)


def their_packet(path, index, k):
    key = (path, index)
    if key not in SPLIT:
        run = subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-i", path, "-map", f"0:{index}", "-c", "copy", "-f", "framecrc", "-"], capture_output=True, text=True)
        sizes = [int(line.split(",")[4]) for line in run.stdout.splitlines() if line and not line.startswith("#")]
        whole = their_bytes(path, index)
        parts, at = [], 0
        for size in sizes:
            parts.append(whole[at:at + size])
            at += size
        SPLIT[key] = parts
    return SPLIT[key][k]


def seek(path, count):
    a, b = ours(path), theirs(path)
    if a is None:
        return False
    video = [(m, y) for m, y in zip(a, b) if m["kind"] == "video" and m["packets"]]
    if not video:
        print(f"seek {os.path.basename(path)}: no video")
        return False
    mine, yours = video[0]
    times = sorted(p[0] for p in mine["packets"])
    keys = sorted(p[0] for p in mine["packets"] if p[4])
    rng = random.Random(3)
    bad = 0
    for _ in range(count):
        target = rng.uniform(times[0], times[-1]) / 1000
        run = subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-ss", f"{target:.3f}", "-i", path, "-map", f"0:{yours['index']}", "-c", "copy", "-copyts",
                              "-f", "framecrc", "-"], capture_output=True, text=True)
        lines = [line for line in run.stdout.splitlines() if line and not line.startswith("#")]
        fields = [int(x) for x in lines[0].split(",")[1:3]] if lines else []
        their_first = (fields[1] if fields[1] != -(1 << 63) else fields[0]) if fields else None
        before = [k for k in keys if k <= target * 1000 + 0.5]
        mine_first = before[-1] if before else keys[0]
        if their_first is None or their_first not in keys or their_first > mine_first:
            bad += 1
            print(f"   seek to {target:.3f} s: our key frame {mine_first} ms, ffmpeg starts at {their_first} ms")
    print(f"seek {os.path.basename(path)}: {count} seeks; ffmpeg starts each on one of our key frames, at or before ours: {count - bad} of {count}")
    return bad == 0


def keys(path, reference, allowed):
    a, b = ours(path), ours(reference)
    if a is None or b is None:
        return False
    for x, y in zip(a, b):
        if x["kind"] != "video":
            continue
        found = [k for k, p in enumerate(x["packets"]) if p[4]]
        real = [k for k, p in enumerate(y["packets"]) if p[4]]
        wrong = [k for k in found if k not in real]
        print(f"keys {os.path.basename(path)}: {len(found)} key frames found through the index, {len(real)} marked in {os.path.basename(reference)}, {len(wrong)} wrong")
        if not found or len(wrong) > allowed * len(found) or (allowed == 0 and found != real):
            return False
    return True


if __name__ == "__main__":
    command = sys.argv[1]
    if command == "packets":
        sys.exit(0 if all(packets(p) for p in sys.argv[2:]) else 1)
    if command == "seek":
        sys.exit(0 if seek(sys.argv[2], int(sys.argv[3])) else 1)
    if command == "keys":
        sys.exit(0 if keys(sys.argv[2], sys.argv[3], float(sys.argv[4])) else 1)
    sys.exit(2)
