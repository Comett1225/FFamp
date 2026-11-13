import hashlib
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.environ.get("AMEDIA", os.path.join(HERE, "..", "build", "ffamp"))
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
FFPROBE = os.environ.get("FFPROBE", "ffprobe")


def ours(path):
    out = subprocess.run([AMEDIA, "packets", path], capture_output=True, text=True, errors="replace")
    if out.returncode != 0:
        return None
    tracks = []
    for line in out.stdout.splitlines():
        words = line.split()
        if not words:
            continue
        if words[0] == "track":
            tracks.append({"id": int(words[1]), "kind": words[2], "codec": words[3], "timescale": int(words[5]), "start": int(words[7]), "end": int(words[9]), "packets": []})
        elif words[0] == "audio":
            tracks[-1]["frame"] = int(words[3]) * int(words[6]) // 8
        elif words[0] in ("format", "video", "tag") or not tracks or len(words) != 6:
            continue
        else:
            tracks[-1]["packets"].append((int(words[0]), int(words[1]), int(words[2]), int(words[3]), words[4] == "K"))
    return tracks


def theirs(path):
    out = subprocess.run([FFPROBE, "-v", "error", "-show_entries", "packet=stream_index,pts_time,duration_time,size,flags", "-of", "csv=p=0", path],
                         capture_output=True, text=True).stdout
    streams = {}
    for line in out.splitlines():
        words = line.split(",")
        if len(words) < 5:
            continue
        number = lambda text: None if text == "N/A" else float(text)
        streams.setdefault(int(words[0]), []).append((number(words[1]), number(words[2]), int(words[3]), "K" in words[4]))
    listed = subprocess.run([FFPROBE, "-v", "error", "-show_entries", "stream=index,codec_type:stream_disposition=attached_pic", "-of", "csv=p=0", path],
                            capture_output=True, text=True).stdout.split()
    kept = []
    for line in listed:
        words = line.split(",")
        if len(words) >= 3 and words[1] in ("video", "audio", "subtitle") and words[2] != "1":
            kept.append(int(words[0]))
    return [(key, [packet for packet in streams.get(key, []) if packet[2] > 0]) for key in kept]


def payload(command):
    return hashlib.md5(subprocess.run(command, capture_output=True).stdout).hexdigest()


def compare(path, tolerance):
    mine = ours(path)
    if mine is None:
        print(f"packets {os.path.basename(path)}: ffamp cannot open it")
        return False
    reference = theirs(path)
    good = len(mine) == len(reference) and len(mine) > 0
    pairs = [(track, stream) for track, stream in zip(mine, reference) if track["codec"] != "...."]
    skipped = len(mine) - len(pairs)
    total = 0
    worst = 0.0
    notes = []
    header = 0
    if path.endswith(".aac"):
        header = 9 if "checksum" in path else 7
    for track, (index, packets) in pairs:
        scale = track["timescale"]
        if len(track["packets"]) != len(packets):
            notes.append(f"track {track['id']} has {len(track['packets'])} packets, ffprobe {len(packets)}")
            good = False
            continue
        total += len(packets)
        if not packets:
            continue
        shift = None
        vorbis = track["codec"] == "vorb"
        allowed = max(tolerance, 0.025) if vorbis else tolerance
        anchor = 1 if vorbis and len(packets) > 1 else 0
        for candidate in (0.0, track["start"] / scale, -(packets[0][1] or 0.0) if vorbis else 0.0):
            if packets[anchor][0] is not None and abs(track["packets"][anchor][0] / scale - candidate - packets[anchor][0]) <= allowed:
                shift = candidate
        if shift is None:
            notes.append(f"track {track['id']} packet {anchor} is at {track['packets'][anchor][0] / scale:.6f} (start {track['start'] / scale:.6f}), ffprobe {packets[anchor][0]}")
            good = False
            continue
        for n, (a, b) in enumerate(zip(track["packets"], packets)):
            if b[0] is not None and n >= anchor:
                difference = abs(a[0] / scale - shift - b[0])
                worst = max(worst, difference)
                if difference > allowed:
                    notes.append(f"track {track['id']} packet {n} pts {a[0] / scale - shift:.6f} ffprobe {b[0]:.6f}")
                    good = False
                    break
            gap = (track["packets"][n + 1][0] - a[0]) / scale if n + 1 < len(packets) else 0.0
            if b[1] is not None and n + 1 < len(packets) and not vorbis and track["kind"] == "audio" and abs(a[2] / scale - b[1]) > tolerance + 1e-6 and abs(gap - b[1]) > tolerance + 1e-6:
                notes.append(f"track {track['id']} packet {n} duration {a[2] / scale:.6f} ffprobe {b[1]:.6f}")
                good = False
                break
            if a[3] + header != b[2] or a[4] != b[3]:
                notes.append(f"track {track['id']} packet {n} size {a[3]} key {a[4]} ffprobe {b[2]} {b[3]}")
                good = False
                break
            if a[1] > a[0] or (n and a[1] < track["packets"][n - 1][1]):
                notes.append(f"track {track['id']} packet {n} dts {a[1]} breaks the order or is above pts {a[0]}")
                good = False
                break
        if not header:
            a = payload([AMEDIA, "packets", path, "--track", str(track["id"]), "--raw"])
            b = payload([FFMPEG, "-nostdin", "-v", "error", "-i", path, "-map", f"0:{index}", "-c", "copy", "-f", "data", "-"])
            if a != b:
                notes.append(f"track {track['id']} packet bytes differ from ffmpeg's")
                good = False
    print(f"packets {os.path.basename(path)}: tracks {len(mine)}/{len(reference)}, unsupported {skipped}, packets {total}, largest pts difference {worst * 1000:.3f} ms" + "".join("; " + note for note in notes))
    return good


def pcm(path):
    mine = ours(path)
    if not mine or len(mine) != 1 or mine[0]["codec"] != "lpcm":
        print(f"pcm {os.path.basename(path)}: ffamp does not give one PCM track")
        return False
    track = mine[0]
    frame = track["frame"]
    packets = track["packets"]
    if not packets:
        print(f"pcm {os.path.basename(path)}: no packets")
        return False
    shaped = all(p[3] == 4096 * frame and p[2] == 4096 for p in packets[:-1]) and 0 < packets[-1][2] <= 4096 and packets[-1][3] == packets[-1][2] * frame
    timed = all(p[0] == n * 4096 for n, p in enumerate(packets))
    frames = sum(p[2] for p in packets)
    signed_bytes = os.path.basename(path).startswith("s8")
    a = payload([AMEDIA, "packets", path, "--raw"])
    b = payload([FFMPEG, "-nostdin", "-v", "error", "-i", path] + (["-c:a", "pcm_u8", "-f", "u8"] if signed_bytes else ["-map", "0:0", "-c", "copy", "-f", "data"]) + ["-"])
    stated = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "a:0", "-show_entries", "stream=duration", "-of", "csv=p=0", path], capture_output=True, text=True).stdout.strip()
    long_enough = stated not in ("", "N/A") and abs(frames / track["timescale"] - float(stated)) < 1e-4
    print(f"pcm {os.path.basename(path)}: packets {len(packets)}, frames {frames}, 4096-frame packets {shaped}, times {timed}, duration matches {long_enough}, bytes match ffmpeg {a == b}")
    return shaped and timed and long_enough and a == b


def granules(path):
    mine = ours(path)
    if not mine or len(mine) != 1:
        print(f"granules {os.path.basename(path)}: ffamp does not give one track")
        return False
    track = mine[0]
    packets = track["packets"]
    data = open(path, "rb").read()
    at = 0
    serial = None
    finished = 0
    run = 0
    pages = []
    while at + 27 <= len(data) and data[at:at + 4] == b"OggS":
        count = data[at + 26]
        table = data[at + 27:at + 27 + count]
        flags = data[at + 5]
        if serial is None:
            serial = data[at + 14:at + 18]
        if data[at + 14:at + 18] == serial:
            if flags & 2 and pages:
                break
            sizes = []
            if not flags & 1:
                run = 0
            for lace in table:
                run += lace
                if lace < 255:
                    sizes.append(run)
                    run = 0
            ended = sum(1 for size in sizes if size > 0)
            finished += ended
            if ended:
                pages.append((finished, struct.unpack("<q", data[at + 6:at + 14])[0]))
            if flags & 4:
                break
        at += 27 + count + sum(table)
    headers = finished - len(packets)
    gaps = []
    for done, granule in pages:
        if done > headers and granule >= 0:
            last = packets[done - headers - 1]
            gaps.append(granule - last[0] - last[2])
    usual = max(set(gaps[:-1]), key=gaps[:-1].count) if len(gaps) > 1 else gaps[0] if gaps else 0
    slack = 1024 if track["codec"] == "vorb" else 0
    steady = headers > 0 and len(gaps) > 0 and all(abs(gap - usual) <= slack for gap in gaps[:-1]) and gaps[-1] <= usual + slack
    offbeat = sum(1 for gap in gaps[:-1] if gap != usual)
    trimmed = len(gaps) > 0 and gaps[-1] < usual
    end_right = not trimmed or track["end"] == pages[-1][1] - usual
    print(f"granules {os.path.basename(path)}: packets {len(packets)}, header packets {headers}, pages checked {len(gaps)}, off by a block change {offbeat}, "
          f"every page end on its granule {steady}, end trim right {end_right}")
    return steady and end_right


if __name__ == "__main__":
    if sys.argv[1] == "granules":
        sys.exit(0 if granules(sys.argv[2]) else 1)
    if sys.argv[1] == "pcm":
        sys.exit(0 if pcm(sys.argv[2]) else 1)
    sys.exit(0 if compare(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 0.00002) else 1)
