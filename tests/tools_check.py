import importlib.util
import os
import subprocess
import sys

import numpy

HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.environ.get("AMEDIA", os.path.join(HERE, "..", "build", "ffamp"))
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")


def run(*args):
    return subprocess.run([AMEDIA] + [str(a) for a in args], capture_output=True, check=True).stdout


def tracks(path):
    found = {}
    current = None
    for line in run("packets", path).decode().splitlines():
        words = line.split()
        if words[0] == "track":
            current = {"timescale": int(words[5]), "start": int(words[7]), "end": int(words[9]), "codec": words[3], "packets": []}
            found.setdefault(words[2], current)
        elif words[0] in ("video", "audio") and current is not None and "width" not in current and words[0] == "video":
            current["width"], current["height"] = [int(v) for v in words[1].split("x")]
        elif current is not None and len(words) == 6 and words[4] in "K_":
            current["packets"].append({"pts": int(words[0]), "dts": int(words[1]), "duration": int(words[2]), "size": int(words[3]), "key": words[4] == "K", "hash": words[5]})
    return found


def shown(track):
    last = track["end"] if track["end"] > track["start"] else 1 << 62
    return sorted(p["pts"] for p in track["packets"] if track["start"] <= p["pts"] < last)


def seconds(track):
    packets = track["packets"]
    natural = max(p["pts"] + p["duration"] for p in packets)
    last = track["end"] if track["end"] > track["start"] else natural
    first = max(track["start"], min(p["pts"] for p in packets))
    return (last - first) / track["timescale"]


def frames(path, width=None, height=None):
    if width is None:
        video = tracks(path)["video"]
        width, height = video["width"], video["height"]
    raw = run("decode", path)
    size = width * height + 2 * ((width + 1) // 2) * ((height + 1) // 2)
    count = len(raw) // size
    return numpy.frombuffer(raw[:count * size], dtype=numpy.uint8).reshape(count, size), width, height


def luma(frame, width, height):
    return frame[:width * height].reshape(height, width).astype(numpy.float64)


def psnr(a, b):
    mse = numpy.mean((a.astype(numpy.float64) - b.astype(numpy.float64)) ** 2)
    return 99.0 if mse == 0 else 10 * numpy.log10(255 * 255 / mse)


def pcm(path, rate=48000):
    return numpy.frombuffer(run("pcm", path, "--rate", rate, "--channels", 2, "--format", "f32le"), dtype=numpy.float32).reshape(-1, 2)


def alike(a, b):
    n = min(len(a), len(b))
    if n == 0:
        return 0.0
    x, y = a[:n].astype(numpy.float64).ravel(), b[:n].astype(numpy.float64).ravel()
    scale = numpy.sqrt(numpy.sum(x * x) * numpy.sum(y * y))
    return float(numpy.sum(x * y) / scale) if scale > 0 else 0.0


def report(ok, text):
    print(("ok   " if ok else "BAD  ") + text)
    return ok


def trim(source, output, start, end):
    start, end = float(start), float(end)
    src, out = tracks(source), tracks(output)
    video = src["video"]
    scale = video["timescale"]
    relative = lambda pts: (pts - video["start"]) / scale
    keys = [p["pts"] for p in video["packets"] if p["key"] and relative(p["pts"]) <= start + 1e-9]
    key = max(keys) if keys else min(p["pts"] for p in video["packets"] if p["key"])
    wanted = [p for p in video["packets"] if p["pts"] >= key and relative(p["pts"]) < end - 1e-9]
    got = out["video"]["packets"]
    hashes = [p["hash"] for p in got]
    ok = report(got and got[0]["key"] and got[0]["hash"] == wanted[0]["hash"], "video starts with the key frame at or before the start")
    ok &= report(all(p["hash"] in hashes for p in wanted), "every video packet in range is there, byte-identical (%d)" % len(wanted))
    order = [p["hash"] for p in video["packets"]]
    at = order.index(hashes[0])
    ok &= report(all(h in order[at:at + len(hashes) + 4] for h in hashes), "no video packet from elsewhere")
    extra = 0 if output.endswith(".mp4") else 2
    ok &= report(0 <= len(shown(out["video"])) - len(wanted) <= extra, "shown frames %d, expected %d" % (len(shown(out["video"])), len(wanted)))
    length = min(end, relative(max(p["pts"] + p["duration"] for p in video["packets"]))) - relative(key)
    ok &= report(abs(seconds(out["video"]) - length) < (1.5 + extra) / 30, "video lasts %.3f s, expected %.3f" % (seconds(out["video"]), length))
    first = min(shown(out["video"]))
    gaps = sorted(set(b - a for a, b in zip(shown(out["video"]), shown(out["video"])[1:])))
    ok &= report(first == out["video"]["start"], "video is shown from its first frame")
    ok &= report(gaps == sorted(set(b - a for a, b in zip(sorted(p["pts"] for p in wanted), sorted(p["pts"] for p in wanted)[1:]))), "frame spacing kept")
    if "audio" in src:
        sound = src["audio"]
        all_hashes = [p["hash"] for p in sound["packets"]]
        cut = [p["hash"] for p in out["audio"]["packets"]]
        at = all_hashes.index(cut[0])
        ok &= report(all_hashes[at:at + len(cut)] == cut, "sound packets are a byte-identical run of the source (%d)" % len(cut))
        ok &= report(abs(seconds(out["audio"]) - length) < 0.002, "sound lasts %.4f s, expected %.4f" % (seconds(out["audio"]), length))
        a = pcm(output)
        offset = int(round(relative(key) * 48000))
        b = pcm(source)[offset:offset + len(a)]
        ok &= report(alike(a[2048:], b[2048:]) > 0.99, "sound lines up with the source at the cut (%.4f)" % alike(a[2048:], b[2048:]))
    return ok


def same_packets(output, kind, source):
    a, b = tracks(output), tracks(source)
    if kind not in a or kind not in b:
        return report(False, "%s track missing" % kind)
    return report([p["hash"] for p in a[kind]["packets"]] == [p["hash"] for p in b[kind]["packets"]], "%s: all %d packets identical" % (kind, len(b[kind]["packets"])))


def merge(video, audio, output):
    ok = same_packets(output, "video", video)
    ok &= same_packets(output, "audio", audio)
    out = tracks(output)
    slack = 0.002 if output.endswith(".mp4") else 0.07
    return ok & report(abs(seconds(out["video"]) - seconds(tracks(video)["video"])) < slack and abs(seconds(out["audio"]) - seconds(tracks(audio)["audio"])) < slack, "lengths kept")


def reverse(source, output, start="0", end="0", floor="30"):
    start, end = float(start), float(end)
    src, width, height = frames(source)
    video = tracks(source)["video"]
    times = [(pts - video["start"]) / video["timescale"] for pts in shown(video)]
    picked = [i for i, t in enumerate(times) if end <= start or start - 1e-7 <= t < end - 1e-7]
    out, out_width, out_height = frames(output)
    ok = report(len(out) == len(picked), "frames %d, expected %d" % (len(out), len(picked)))
    ok &= report((out_width, out_height) == ((width + 1) // 2 * 2, (height + 1) // 2 * 2), "size %dx%d" % (out_width, out_height))
    if not ok or (out_width, out_height) != (width, height):
        return ok
    back = [psnr(luma(out[n], width, height), luma(src[picked[-1 - n]], width, height)) for n in range(len(out))]
    forward = [psnr(luma(out[n], width, height), luma(src[picked[n]], width, height)) for n in range(len(out))]
    ok &= report(min(back) >= float(floor), "frame N equals source frame (last - N): PSNR mean %.1f dB, lowest %.1f dB" % (numpy.mean(back), min(back)))
    ok &= report(numpy.mean(back) > numpy.mean(forward) + 5, "and not the forward order (%.1f dB)" % numpy.mean(forward))
    out_video = tracks(output)["video"]
    stamps = shown(video)
    final = [p["duration"] for p in video["packets"] if p["pts"] == stamps[-1]][0]
    held = [(stamps[i + 1] if i + 1 < len(stamps) else stamps[i] + final) - stamps[i] for i in picked]
    out_gaps = [b - a for a, b in zip(shown(out_video), shown(out_video)[1:])]
    ok &= report(out_gaps == held[::-1][:-1], "every frame is shown as long as in the source, in backwards order")
    ok &= report(abs(seconds(out_video) - sum(held) / video["timescale"]) < 0.001, "length %.3f s" % seconds(out_video))
    if "audio" in tracks(source):
        first = times[picked[0]]
        last = times[picked[-1] + 1] if picked[-1] + 1 < len(times) else seconds(video)
        a = pcm(output)
        b = pcm(source)[int(round(first * 48000)):int(round(last * 48000))][::-1]
        ok &= report(abs(len(a) - len(b)) <= 2, "sound has %d samples, expected %d" % (len(a), len(b)))
        ok &= report(alike(a, b) > 0.98, "sound is the source backwards (%.4f), not forwards (%.4f)" % (alike(a, b), alike(a, b[::-1])))
    return ok


def gif_module():
    spec = importlib.util.spec_from_file_location("gif_make", os.path.join(HERE, "fixtures", "tools", "gif_make.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def gif_expected(name):
    module = gif_module()
    scene = [s for s in module.scenes() if s["name"] == name][0]
    canvases = []
    for canvas in module.composite(scene):
        rgba = numpy.array(canvas, dtype=numpy.uint8)
        bgra = rgba[:, :, [2, 1, 0, 3]].copy()
        bgra[rgba[:, :, 3] == 0] = 0
        canvases.append(bgra)
    return scene, canvases, module.delays(scene)


def gif_frames(path, name, scratch):
    scene, canvases, delays = gif_expected(name)
    text = run("gif-info", path, "--frames", scratch).decode().splitlines()
    got = numpy.fromfile(scratch, dtype=numpy.uint8)
    os.remove(scratch)
    ok = report(text[1].split()[1:] == [str(d) for d in delays], "delays " + " ".join(str(d) for d in delays))
    expected = numpy.concatenate([c.ravel() for c in canvases])
    ok &= report(len(got) == len(expected) and numpy.array_equal(got, expected), "all %d frames equal the reference drawing, colour and alpha, to the byte" % len(canvases))
    return ok


def gif_video(name, colour, matte):
    scene, canvases, delays = gif_expected(name)
    width, height = scene["width"], scene["height"]
    even = lambda v: max(2, int(v / 2 + 0.5) * 2)
    wide, tall = even(width), even(height)
    xs = numpy.minimum((numpy.arange(wide) * 2 + 1) * width // (wide * 2), width - 1)
    ys = numpy.minimum((numpy.arange(tall) * 2 + 1) * height // (tall * 2), height - 1)
    transparent = any((c[:, :, 3] != 255).any() for c in canvases)
    ok = report(os.path.exists(matte) == transparent, "matte file %s" % ("written" if transparent else "not written, nothing is transparent"))
    track = tracks(colour)["video"]
    ok &= report((track["width"], track["height"]) == (wide, tall), "video size %dx%d" % (wide, tall))
    starts = [sum(delays[:i]) for i in range(len(delays))]
    ok &= report([(pts - track["start"]) * 1000 // track["timescale"] for pts in shown(track)] == starts, "frame times follow the delays")
    ok &= report(abs(seconds(track) * 1000 - sum(delays)) < 1.5, "length %.0f ms, expected %d" % (seconds(track) * 1000, sum(delays)))
    got, _, _ = frames(colour, wide, tall)
    ok &= report(len(got) == len(canvases), "colour frames %d" % len(got))
    worst = 99.0
    for frame, canvas in zip(got, canvases):
        scaled = canvas[ys][:, xs].astype(numpy.float64)
        y = 16 + 219 * (0.2126 * scaled[:, :, 2] + 0.7152 * scaled[:, :, 1] + 0.0722 * scaled[:, :, 0]) / 255
        worst = min(worst, psnr(luma(frame, wide, tall), y))
    ok &= report(worst > 30, "colour frames are the premultiplied drawing: lowest luma PSNR %.1f dB" % worst)
    if transparent and os.path.exists(matte):
        got, _, _ = frames(matte, wide, tall)
        mask = tracks(matte)["video"]
        ok &= report(len(got) == len(canvases) and [p - mask["start"] for p in shown(mask)] == [p - track["start"] for p in shown(track)], "matte has the same frames and times")
        worst = 99.0
        for frame, canvas in zip(got, canvases):
            y = 16 + 219 * canvas[ys][:, xs][:, :, 3].astype(numpy.float64) / 255
            worst = min(worst, psnr(luma(frame, wide, tall), y))
        ok &= report(worst > 30, "matte luma is the alpha: lowest PSNR %.1f dB" % worst)
    return ok


def shrink(picture, width, height, to_width, to_height):
    if width % to_width or height % to_height:
        return None
    fx, fy = width // to_width, height // to_height
    return picture.reshape(to_height, fy, to_width, fx).mean(axis=(1, 3))


def compress(source, output, width, height, fps, start, end, bitrate, sound, tolerance="0.35"):
    width, height, fps, start, end, bitrate = int(width), int(height), float(fps), float(start), float(end), int(bitrate)
    src = tracks(source)
    out = tracks(output)
    video = out["video"]
    source_video = src["video"]
    length = (end - start) if end > start else seconds(source_video) - start
    ok = report((video["width"], video["height"]) == (width, height), "size %dx%d" % (video["width"], video["height"]))
    count = len(shown(video))
    expected = length * fps
    ok &= report(abs(count - expected) <= 1, "frames %d, expected %.1f" % (count, expected))
    gaps = [b - a for a, b in zip(shown(video), shown(video)[1:])]
    steady = max(gaps) - min(gaps) <= (1 if video["timescale"] == 1000 else 0) if gaps else True
    ok &= report(steady and (not gaps or abs(video["timescale"] * len(gaps) / sum(gaps) - fps) < 0.05), "constant %.3f frames a second" % fps)
    ok &= report(abs(seconds(video) - length) <= 1.0 / fps + 0.001, "video lasts %.3f s, expected %.3f" % (seconds(video), length))
    ok &= report(video["packets"][0]["key"] and min(shown(video)) == video["start"], "starts at zero on a key frame")
    rate = sum(p["size"] for p in video["packets"]) * 8 / seconds(video)
    ok &= report(abs(rate - bitrate) <= bitrate * float(tolerance), "video bitrate %.0f, asked %d (within %.0f %%)" % (rate, bitrate, float(tolerance) * 100))
    if sound == "none":
        ok &= report("audio" not in out, "no sound")
    elif "audio" in src:
        ok &= report("audio" in out and abs(seconds(out["audio"]) - min(length, seconds(src["audio"]) - start)) < 0.03, "sound lasts %.3f s" % (seconds(out["audio"]) if "audio" in out else 0))
        if "audio" in out:
            a = pcm(output)
            b = pcm(source)[int(round(start * 48000)):][:len(a)]
            ok &= report(alike(a[4096:], b[4096:]) > 0.97, "sound lines up with the source (%.4f)" % alike(a[4096:], b[4096:]))
            if sound == "copy":
                all_hashes = [p["hash"] for p in src["audio"]["packets"]]
                ok &= report(all(p["hash"] in all_hashes for p in out["audio"]["packets"]), "sound packets are copied byte-identical")
    got, _, _ = frames(output, width, height)
    original, source_width, source_height = frames(source)
    times = [(pts - source_video["start"]) / source_video["timescale"] for pts in shown(source_video)]
    scores = []
    others = []
    for k in range(min(len(got), count)):
        moment = start + k / fps
        index = max(i for i, t in enumerate(times) if t <= moment + 0.5 / fps + 1e-9 and (t >= start - 1e-9 or i == 0) or i == 0)
        small = shrink(luma(original[index], source_width, source_height), source_width, source_height, width, height)
        if small is None:
            return ok
        scores.append(psnr(luma(got[k], width, height), small))
        far = shrink(luma(original[(index + 7) % len(original)], source_width, source_height), source_width, source_height, width, height)
        others.append(psnr(luma(got[k], width, height), far))
    ok &= report(numpy.mean(scores) > numpy.mean(others) + 2 and numpy.mean(scores) > 22, "pictures are the right source frames: PSNR %.1f dB against %.1f dB for frames 7 away" % (numpy.mean(scores), numpy.mean(others)))
    return ok


def reference_frames(path, width, height, mode):
    raw = subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-i", path, "-map", "0:v:0", "-fps_mode", "passthrough", "-pix_fmt", "rgb24" if mode == "rgb" else "yuv420p", "-f", "rawvideo", "-"],
                         capture_output=True, check=True).stdout
    if mode != "rgb":
        size = width * height * 3 // 2
        return numpy.frombuffer(raw[:len(raw) // size * size], dtype=numpy.uint8).reshape(-1, size)[:, :width * height]
    size = width * height * 3
    rgb = numpy.frombuffer(raw[:len(raw) // size * size], dtype=numpy.uint8).reshape(-1, height * width, 3).astype(numpy.float64)
    return rgb[:, :, 0] * 0.2126 + rgb[:, :, 1] * 0.7152 + rgb[:, :, 2] * 0.0722


def gif_drawing(name, path, matrix):
    scene, canvases, delays = gif_expected(name)
    track = tracks(path)["video"]
    wide, tall = track["width"], track["height"]
    xs = numpy.minimum((numpy.arange(wide) * 2 + 1) * scene["width"] // (wide * 2), scene["width"] - 1)
    ys = numpy.minimum((numpy.arange(tall) * 2 + 1) * scene["height"] // (tall * 2), scene["height"] - 1)
    weights = (0.2126, 0.7152, 0.0722) if matrix == "709" else (0.299, 0.587, 0.114)
    raw = subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-i", path, "-map", "0:v:0", "-fps_mode", "passthrough", "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"], capture_output=True, check=True).stdout
    size = wide * tall * 3 // 2
    worst = 99.0
    for i, canvas in enumerate(canvases):
        scaled = canvas[ys][:, xs].astype(numpy.float64)
        y = 16 + 219 * (weights[0] * scaled[:, :, 2] + weights[1] * scaled[:, :, 1] + weights[2] * scaled[:, :, 0]) / 255
        got = numpy.frombuffer(raw[i * size:i * size + wide * tall], dtype=numpy.uint8)
        if len(got) != wide * tall:
            return report(False, "frame %d missing" % i)
        worst = min(worst, psnr(got.reshape(tall, wide), y))
    return report(worst > 30, "decoded by FFmpeg, every frame is the premultiplied drawing: lowest luma PSNR %.1f dB" % worst)


def against(ours, theirs, floor="30", mode="yuv", length_slack="0.07", times="1"):
    a, b = tracks(ours)["video"], tracks(theirs)["video"]
    ok = report((a["width"], a["height"]) == (b["width"], b["height"]), "same size")
    ok &= report(len(shown(a)) == len(shown(b)), "frames %d against %d" % (len(shown(a)), len(shown(b))))
    ok &= report(abs(seconds(a) - seconds(b)) <= float(length_slack), "length %.3f s against %.3f s" % (seconds(a), seconds(b)))
    scale = lambda track: [round((pts - track["start"]) * 1000 / track["timescale"]) for pts in shown(track)]
    if times == "1":
        ok &= report(all(abs(x - y) <= 1 for x, y in zip(scale(a), scale(b))), "frame times agree to the millisecond")
    if mode == "none":
        return ok
    if mode == "yuv":
        x, y = frames(ours)[0], frames(theirs)[0]
    else:
        x, y = reference_frames(ours, a["width"], a["height"], mode), reference_frames(theirs, a["width"], a["height"], mode)
    n = min(len(x), len(y))
    score = numpy.mean([psnr(x[i], y[i]) for i in range(n)])
    ok &= report(score >= float(floor), "pictures agree: %.1f dB" % score)
    return ok


def quality(source, ours, theirs, start, fps):
    start, fps = float(start), float(fps)
    original, source_width, source_height = frames(source)
    video = tracks(source)["video"]
    times = [(pts - video["start"]) / video["timescale"] for pts in shown(video)]
    scores = []
    for path in (ours, theirs):
        got, width, height = frames(path)
        values = []
        for k in range(len(got)):
            index = max([i for i, t in enumerate(times) if start - 1e-9 <= t <= start + k / fps + 1e-9] or [0])
            values.append(psnr(luma(got[k], width, height), shrink(luma(original[index], source_width, source_height), source_width, source_height, width, height)))
        scores.append(numpy.mean(values))
    return report(scores[0] >= scores[1] - 1.0, "against the source frames: ours %.1f dB, FFmpeg's %.1f dB" % (scores[0], scores[1]))


def subset(ours, theirs, kind):
    a, b = tracks(ours), tracks(theirs)
    mine = [p["hash"] for p in a[kind]["packets"]]
    other = [p["hash"] for p in b[kind]["packets"]]
    ok = report(all(h in other for h in mine), "%s: each of our %d packets is among the reference's %d" % (kind, len(mine), len(other)))
    if kind == "video":
        ok &= report(mine[0] == other[0], "video: same first packet")
    return ok


def sound_like(ours, theirs, bitrate):
    a, b = tracks(ours)["audio"], tracks(theirs)["audio"]
    ok = report(abs(seconds(a) - seconds(b)) < 0.06, "length %.3f s against %.3f s" % (seconds(a), seconds(b)))
    rate = sum(p["size"] for p in a["packets"]) * 8 / seconds(a)
    ok &= report(abs(rate - int(bitrate)) < int(bitrate) * 0.2, "bitrate %.0f, asked %s" % (rate, bitrate))
    x, y = pcm(ours), pcm(theirs)
    ok &= report(alike(x[4096:], y[4096:]) > 0.97, "sound agrees (%.4f)" % alike(x[4096:], y[4096:]))
    return ok


def starts(path):
    import json
    found = {}
    for stream in json.loads(run("probe", "--json", path))["streams"]:
        found.setdefault(stream["codec_type"], float(stream.get("start_time", 0)))
    return found


def sync(source, output, video_source=None):
    a, b = starts(source), starts(output)
    if video_source:
        gap = 0.0
    else:
        gap = a["video"] - a["audio"]
    kept = b["video"] - b["audio"]
    ok = report(abs(gap - kept) < 0.0015, "picture starts %.4f s after the sound, source %.4f s" % (kept, gap))
    return ok & report(min(b.values()) == 0, "the file starts at zero")


def sound_kept(source, output, bitrate, floor="0.97"):
    a = tracks(output)["audio"]
    b = tracks(source)["audio"]
    ok = report(abs(seconds(a) - seconds(b)) < 0.03, "length %.3f s, source %.3f s" % (seconds(a), seconds(b)))
    rate = sum(p["size"] for p in a["packets"]) * 8 / seconds(a)
    ok &= report(abs(rate - int(bitrate)) < int(bitrate) * 0.2, "bitrate %.0f, asked %s" % (rate, bitrate))
    x, y = pcm(output), pcm(source)
    n = min(len(x), len(y))
    best = max(alike(x[4096 + shift:n - 4096], y[4096:n - 4096 - shift]) for shift in range(0, 1200, 24)) if n > 12000 else 0
    return ok & report(alike(x[4096:], y[4096:]) > float(floor) or best > float(floor), "sound agrees with the source (%.4f)" % max(best, alike(x[4096:], y[4096:])))


def quiet_until(path, moment):
    moment = float(moment)
    sound = pcm(path)
    before = numpy.abs(sound[:int((moment - 0.03) * 48000)]).max()
    after = numpy.abs(sound[int((moment + 0.05) * 48000):int((moment + 0.3) * 48000)]).max()
    return report(before < 0.01 and after > 0.05, "silence up to %.3f s (peak %.4f), sound after it (peak %.3f)" % (moment, before, after))


def progress(log):
    values = [float(line.split()[1]) for line in open(log).read().splitlines() if line.startswith("progress ")]
    ok = report(len(values) >= 3 and values == sorted(values), "%d progress lines, never going back" % len(values))
    return ok & report(bool(values) and values[-1] == 1.0 and all(0 <= v <= 1 for v in values), "ends at 1.00")


if __name__ == "__main__":
    command = sys.argv[1].replace("-", "_")
    try:
        result = globals()[command](*sys.argv[2:])
    except subprocess.CalledProcessError as problem:
        result = report(False, "command failed: %s" % problem.stderr.decode(errors="replace").strip())
    sys.exit(0 if result else 1)
