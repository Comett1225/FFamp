import os
import subprocess
import sys

import numpy

HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.environ.get("AMEDIA", os.path.join(HERE, "..", "build", "ffamp"))
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
FFPROBE = os.environ.get("FFPROBE", "ffprobe")


def shape(path):
    words = subprocess.run([AMEDIA, "videoinfo", path], capture_output=True, text=True, check=True).stdout.split()
    found = dict(zip(words[0::2], words[1::2]))
    width, height = (int(v) for v in found["size"].split("x"))
    cw, ch = (int(v) for v in found["chroma"].split("x"))
    return width, height, cw, ch


def planes(path, kind):
    width, height, cw, ch = shape(path)
    ours = subprocess.run([AMEDIA, "decode", path], capture_output=True).stdout
    theirs = subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-i", path, "-map", "0:v:0", "-fps_mode", "passthrough", "-pix_fmt", kind, "-f", "rawvideo", "-"],
                            capture_output=True).stdout
    frame = width * height + 2 * cw * ch
    count = len(ours) // frame
    got = numpy.frombuffer(ours[:count * frame], numpy.uint8).reshape(count, frame)
    if kind == "gray":
        got = got[:, :width * height]
        want = numpy.frombuffer(theirs, numpy.uint8).reshape(-1, width * height)
    else:
        v = numpy.frombuffer(theirs, "<u2").reshape(-1, height, width).astype(numpy.uint32)
        r, g, b = (v >> 10) & 31, (v >> 5) & 31, v & 31
        grow = lambda c: ((c << 3) | (c >> 2)).astype(numpy.uint8)
        want = numpy.concatenate([grow(g).reshape(len(v), -1), grow(b).reshape(len(v), -1), grow(r).reshape(len(v), -1)], axis=1)
    same = len(want) == count and numpy.array_equal(got, want)
    print(f"planes {os.path.basename(path)} against ffmpeg's {kind}: frames {count} and {len(want)}, identical {same}")
    return count > 0 and same


def times(path):
    ours = [float(x) for x in subprocess.run([AMEDIA, "decode", path, "--times"], capture_output=True, text=True).stdout.split()]
    listed = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0", "-show_entries", "frame=best_effort_timestamp_time", "-of", "csv=p=0", path],
                            capture_output=True, text=True).stdout.replace(",", " ").split()
    theirs = [None if x == "N/A" else float(x) for x in listed]
    known = [(a, b) for a, b in zip(ours, theirs) if b is not None]
    gaps = [round(b - a, 4) for a, b in known]
    shift = max(set(gaps), key=gaps.count) if gaps else 0.0
    off = [n for n, (a, b) in enumerate(known) if abs(a + shift - b) >= 0.0005]
    print(f"times {os.path.basename(path)}: {len(ours)} frames against ffprobe's {len(theirs)} ({len(theirs) - len(known)} without a time), constant shift {shift:.6f} s, "
          f"frames off that shift {off[:5]}")
    return len(ours) == len(theirs) and len(ours) > 0 and (not off or off == [0])


def starts(path):
    import json
    ours = json.loads(subprocess.run([AMEDIA, "probe", "--json", path], capture_output=True, text=True).stdout)
    good = True
    notes = []
    for stream in ours["streams"]:
        first = subprocess.run([FFPROBE, "-v", "error", "-select_streams", str(stream["index"]), "-show_entries", "packet=pts_time,dts_time", "-read_intervals", "%+#1",
                                "-of", "csv=p=0", path], capture_output=True, text=True).stdout.split()
        values = [float(x) for x in first[0].split(",") if x not in ("N/A", "")] if first else []
        want = values[0] if values else 0.0
        got = float(stream.get("start_time", "0"))
        notes.append(f"stream {stream['index']} {got:.6f} against {want:.6f}")
        good = good and abs(got - want) < 0.0005
    print(f"starts {os.path.basename(path)}: " + ", ".join(notes))
    return good


def far(path, count, seed, kind):
    import random
    width, height, cw, ch = shape(path)
    size = width * height + 2 * cw * ch
    words = subprocess.run([AMEDIA, "videoinfo", path], capture_output=True, text=True, check=True).stdout.split()
    found = dict(zip(words[0::2], words[1::2]))
    frames = int(found["frames"])
    rate = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=r_frame_rate", "-of", "csv=p=0", path],
                          capture_output=True, text=True).stdout.strip().strip(",").split("/")
    fps = float(rate[0]) / float(rate[1] if len(rate) > 1 else 1)
    rng = random.Random(seed)
    picks = sorted(set([0, frames - 1] + [rng.randrange(frames) for _ in range(count)]))
    ours = subprocess.run([AMEDIA, "frames", path, "--list", ",".join(str(n) for n in picks)], capture_output=True).stdout
    exact = 0
    for i, n in enumerate(picks):
        at = max(n / fps - 0.001, 0.0)
        want = subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-ss", f"{at:.4f}", "-i", path, "-map", "0:v:0", "-frames:v", "1", "-pix_fmt", kind, "-f", "rawvideo", "-"],
                              capture_output=True).stdout
        exact += ours[i * size:(i + 1) * size] == want
    judge = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0", "-count_packets", "-show_entries", "stream=nb_read_packets", "-of", "csv=p=0", path],
                           capture_output=True, text=True).stdout.strip().strip(",")
    print(f"far {os.path.basename(path)}: {frames} frames (ffprobe reads {judge} packets), {exact}/{len(picks)} frames across {os.path.getsize(path) >> 20} MiB exact")
    return str(frames) == judge and exact == len(picks)


if __name__ == "__main__":
    kind, rest = sys.argv[1], sys.argv[2:]
    if kind == "planes":
        ok = planes(rest[0], rest[1])
    elif kind == "times":
        ok = times(rest[0])
    elif kind == "starts":
        ok = starts(rest[0])
    elif kind == "far":
        ok = far(rest[0], int(rest[1]), int(rest[2]), rest[3])
    else:
        ok = False
    sys.exit(0 if ok else 1)
