import os
import random
import subprocess
import sys
import time
import wave

import numpy as np


HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.path.join(HERE, "..", "build", "amedia")


def amedia(*arguments):
    return subprocess.run([AMEDIA] + [str(a) for a in arguments], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout


def raw(path):
    return np.fromfile(path, dtype=np.uint8).astype(np.float64)


def psnr_of(a, b):
    if a.size != b.size or a.size == 0:
        return -1.0
    error = ((a - b) ** 2).mean()
    return 99.0 if error == 0 else 10 * np.log10(255 * 255 / error)


def psnr(first, second, least):
    value = psnr_of(raw(first), raw(second))
    print("      psnr %.2f dB (needs %s)" % (value, least))
    return value >= float(least)


def same(first, second):
    a, b = raw(first), raw(second)
    return a.size > 0 and a.size == b.size and bool((a == b).all())


def swapped(first, second, layout):
    a = np.fromfile(first, dtype=np.uint8).reshape(-1, 4)
    b = np.fromfile(second, dtype=np.uint8)
    if layout == "rgba":
        return a.size == b.size and bool((a[:, [2, 1, 0, 3]].reshape(-1) == b).all())
    return a.shape[0] * 3 == b.size and bool((a[:, [2, 1, 0]].reshape(-1) == b).all())


def write_wav(path, rate, samples):
    with wave.open(path, "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes((np.clip(samples, -1, 1) * 32767).astype("<i2").tobytes())


def read_wav(path):
    with wave.open(path, "rb") as source:
        if source.getsampwidth() != 2:
            raise SystemExit("not 16-bit")
        data = np.frombuffer(source.readframes(source.getnframes()), dtype="<i2").astype(np.float64) / 32768
        return source.getframerate(), source.getnchannels(), data


def tone(path, rate, seconds, frequency):
    t = np.arange(int(float(seconds) * int(rate))) / int(rate)
    write_wav(path, int(rate), 0.5 * np.sin(2 * np.pi * float(frequency) * t))
    return True


def voiced(path, rate, seconds, frequency):
    rate = int(rate)
    t = np.arange(int(float(seconds) * rate)) / rate
    glide = float(frequency) * (1 + 0.08 * np.sin(2 * np.pi * 0.7 * t))
    phase = 2 * np.pi * np.cumsum(glide) / rate
    sound = sum(np.sin(k * phase) / k for k in range(1, 9))
    syllables = 0.55 + 0.45 * np.sin(2 * np.pi * 3.1 * t)
    write_wav(path, rate, 0.25 * sound * syllables)
    return True


def peak_frequency(data, rate):
    window = np.hanning(data.size)
    spectrum = np.abs(np.fft.rfft(data * window))
    k = int(spectrum[1:].argmax()) + 1
    a, b, c = np.log(spectrum[k - 1] + 1e-12), np.log(spectrum[k] + 1e-12), np.log(spectrum[min(k + 1, spectrum.size - 1)] + 1e-12)
    shift = 0.5 * (a - c) / (a - 2 * b + c) if a - 2 * b + c != 0 else 0
    return (k + shift) * rate / data.size, spectrum, k


def pitch_track(data, rate, low=60, high=500):
    size = int(rate * 0.04)
    found = []
    for start in range(0, data.size - size * 2, size):
        piece = data[start:start + size * 2]
        if np.sqrt((piece ** 2).mean()) < 0.02:
            continue
        piece = piece - piece.mean()
        spectrum = np.fft.rfft(piece, piece.size * 2)
        lags = np.fft.irfft(np.abs(spectrum) ** 2)
        first, last = int(rate / high), int(rate / low)
        best = first + int(lags[first:last].argmax())
        if lags[best] > 0.3 * lags[0]:
            found.append(rate / best)
    return np.median(found) if found else 0.0


def speech_tone(source, result, speed, pitch, frequency, slack=1):
    speed, pitch, frequency = float(speed), float(pitch), float(frequency)
    in_rate, _, before = read_wav(source)
    rate, channels, after = read_wav(result)
    wanted = int(np.floor(before.size * rate / in_rate / speed + 0.5))
    ok = channels == 1 and abs(after.size - wanted) <= int(slack)
    edge = int(rate * 0.1)
    body = after[edge:after.size - edge]
    measured, spectrum, k = peak_frequency(body, rate)
    power = spectrum ** 2
    width = max(3, int(k * 0.02))
    inside = power[k - width:k + width + 1].sum()
    noise = power.sum() - inside
    purity = 10 * np.log10(inside / max(noise, 1e-20))
    level = 20 * np.log10(np.sqrt((body ** 2).mean()) / np.sqrt((before ** 2).mean()))
    jump = np.abs(np.diff(body)).max()
    smooth = 0.5 * 2 * np.pi * frequency * pitch / rate
    print("      samples %d (wanted %d) frequency %.2f Hz (wanted %.2f) purity %.1f dB level %+.2f dB largest step %.4f (pure tone %.4f)" % (after.size, wanted, measured, frequency * pitch, purity, level, jump, smooth))
    return ok and abs(measured / (frequency * pitch) - 1) < 0.004 and purity > 30 and abs(level) < 1.0 and jump < smooth * 1.6 + 0.004


def speech_voice(source, result, speed, pitch, slack=0.04):
    speed, pitch = float(speed), float(pitch)
    in_rate, _, before = read_wav(source)
    rate, channels, after = read_wav(result)
    wanted = int(np.floor(before.size * rate / in_rate / speed + 0.5))
    was, now = pitch_track(before, in_rate), pitch_track(after, rate)
    level = 20 * np.log10(np.sqrt((after ** 2).mean()) / np.sqrt((before ** 2).mean()))
    ratio = now / was if was else 0
    print("      samples %d (wanted %d) pitch %.1f Hz -> %.1f Hz (ratio %.3f, wanted %.3f) level %+.2f dB" % (after.size, wanted, was, now, ratio, pitch, level))
    return channels == 1 and abs(after.size - wanted) <= 1 and abs(ratio / pitch - 1) < float(slack) and abs(level) < 1.5


def pattern(path, width, height, frames, layout):
    width, height, frames = int(width), int(height), int(frames)
    y, x = np.mgrid[0:height, 0:width].astype(np.float64)
    with open(path, "wb") as out:
        for n in range(frames):
            cx, cy = width * (0.3 + 0.4 * n / max(frames - 1, 1)), height * 0.5
            blob = np.exp(-((x - cx) ** 2 + (y - cy) ** 2) / (2 * (height * 0.18) ** 2))
            red = 40 + 150 * x / width + 60 * blob
            green = 60 + 120 * y / height + 70 * blob
            blue = 200 - 120 * x / width - 40 * y / height + 50 * blob
            picture = np.clip(np.stack([red, green, blue], axis=2), 0, 255).astype(np.uint8)
            if layout == "gray":
                out.write(picture[:, :, 1].tobytes())
            elif layout == "rgb24":
                out.write(picture.tobytes())
            else:
                alpha = np.full((height, width, 1), 255, np.uint8)
                order = [2, 1, 0] if layout == "bgra" else [0, 1, 2]
                out.write(np.concatenate([picture[:, :, order], alpha], axis=2).tobytes())
    return True


def recolour(first, second, layout, least):
    a = np.fromfile(first, dtype=np.uint8)
    b = np.fromfile(second, dtype=np.uint8)
    if layout == "bgra":
        a = a.reshape(-1, 4)[:, [2, 1, 0, 3]].reshape(-1)
    if layout == "rgb24":
        b = b.reshape(-1, 4)[:, :3].reshape(-1)
    value = psnr_of(a.astype(np.float64), b.astype(np.float64))
    print("      psnr %.2f dB (needs %s)" % (value, least))
    return value >= float(least)


def luma(grey, planar, width, height, least):
    width, height = int(width), int(height)
    wanted = np.fromfile(grey, dtype=np.uint8).reshape(-1, height, width).astype(np.float64)
    size = width * height + 2 * ((width + 1) // 2) * ((height + 1) // 2)
    got = np.fromfile(planar, dtype=np.uint8)
    if got.size != wanted.shape[0] * size:
        return False
    got = got.reshape(-1, size)[:, :width * height].reshape(-1, height, width).astype(np.float64)
    value = psnr_of(np.floor(16 + (wanted * 219 + 127) / 255), got)
    chroma = np.fromfile(planar, dtype=np.uint8).reshape(-1, size)[:, width * height:].astype(np.float64)
    drift = np.abs(chroma - 128).max()
    print("      luma psnr %.2f dB (needs %s), chroma within %d of neutral" % (value, least, drift))
    return value >= float(least) and drift <= 3


def randomly(path, size, layout, rate):
    whole = amedia("frames-out", path, "--size", size, "--format", layout)
    total = int(amedia("frames-out", path, "--info").split()[7])
    if total < 4 or len(whole) % total:
        return False
    step = len(whole) // total
    rng = random.Random(5)
    for n in [0, total - 1] + [rng.randrange(total) for _ in range(8)]:
        wanted = whole[n * step:(n + 3) * step]
        if amedia("frames-out", path, "--size", size, "--format", layout, "--frame", n, "--count", 3) != wanted:
            print("      frame", n, "differs when asked for by number")
            return False
        if amedia("frames-out", path, "--size", size, "--format", layout, "--start", "%.6f" % (n / float(rate)), "--count", 3) != wanted:
            print("      frame", n, "differs when asked for by time")
            return False
        if n and amedia("frames-out", path, "--size", size, "--format", layout, "--start", "%.6f" % ((n - 0.7) / float(rate)), "--count", 1) != wanted[:step]:
            print("      a time before frame", n, "did not give that frame")
            return False
    return True


def steady(path, timescale, step_ticks, fps, start, length, filled):
    timescale, step_ticks, fps, start, length = int(timescale), int(step_ticks), float(fps), float(start), float(length)
    whole = amedia("frames-out", path, "--format", "i420")
    total = int(amedia("frames-out", path, "--info").split()[7])
    step = len(whole) // total
    got = amedia("frames-out", path, "--format", "i420", "--fps", fps, "--start", start, "--length", length)
    cut = int(np.floor(start * timescale + 0.5))
    ticks = [n * step_ticks - cut for n in range(total + 1)]
    nearest = lambda t: int(np.floor(abs(t) * fps / timescale + 0.5)) * (1 if t >= 0 else -1)
    index = next(i for i in range(total) if i * step_ticks / timescale >= start - 5e-7)
    opening = nearest(ticks[index])
    end = max(nearest(ticks[total]), opening + 1)
    wanted = []
    for n in range(0 if int(filled) else opening, min(end, int(np.floor(length * fps + 0.5)))):
        while index + 1 < total and nearest(ticks[index + 1]) <= max(n, opening):
            index += 1
        wanted.append(index)
    print("      %d frames at %.3f fps from %d source frames" % (len(wanted), fps, len(set(wanted))))
    return got == b"".join(whole[i * step:(i + 1) * step] for i in wanted)


def turned(plain, rotated, width, height, rotation):
    width, height, rotation = int(width), int(height), int(rotation)
    a = np.fromfile(plain, dtype=np.uint8).reshape(-1, height, width, 4)
    b = np.fromfile(rotated, dtype=np.uint8)
    wanted = np.rot90(a, k=-(rotation // 90), axes=(1, 2))
    return wanted.size == b.size and bool((wanted.reshape(-1) == b).all())


def sound_like(first, second, least):
    a = np.fromfile(first, dtype="<i2").astype(np.float64)
    b = np.fromfile(second, dtype="<i2").astype(np.float64)
    if a.size == 0 or abs(a.size - b.size) > 2:
        print("      lengths", a.size, b.size)
        return False
    count = min(a.size, b.size)
    a, b = a[:count], b[:count]
    noise = ((a - b) ** 2).mean()
    value = 99.0 if noise == 0 else 10 * np.log10((a ** 2).mean() / noise)
    print("      %d samples, signal to difference %.1f dB (needs %s)" % (count, value, least))
    return value >= float(least)


def sync(path, least_frames, least_seconds):
    info = amedia("frames-out", path, "--info").split()
    total, length = int(info[7]), float(info[11])
    pictures = np.frombuffer(amedia("frames-out", path, "--size", "32x18", "--format", "i420"), dtype=np.uint8)
    if total == 0 or pictures.size != total * 864:
        return False
    bright = pictures.reshape(total, 864)[:, :576].mean(axis=1) > 150
    sound = np.frombuffer(amedia("pcm", path, "--rate", 48000, "--channels", 1, "--format", "s16le"), dtype="<i2").astype(np.float64) / 32768
    loud = np.abs(sound) > 0.1
    flashes = [n / 30 for n in range(1, total) if bright[n] and not bright[n - 1]]
    edges = np.nonzero(loud)[0]
    beeps = []
    for index in edges:
        if not beeps or index - last > 9600:
            beeps.append(index / 48000)
        last = index
    beeps = [b for b in beeps if b > 0.05]
    pairs = [(f, min(beeps, key=lambda b: abs(b - f))) for f in flashes] if beeps else []
    worst = max((abs(f - b) for f, b in pairs), default=9)
    print("      %d pictures, %.3f s of picture, %.3f s of sound, %d flashes, %d beeps, worst flash to beep distance %.1f ms" % (total, length, sound.size / 48000, len(flashes), len(beeps), worst * 1000))
    return total >= int(least_frames) and length >= float(least_seconds) and len(pairs) >= 2 and worst < 0.025 and abs(sound.size / 48000 - length) < 0.12


def fuzz(path, count, seed):
    original = open(path, "rb").read()
    rng = random.Random(int(seed))
    scratch = os.path.join(HERE, "out", "capfuzz_" + os.path.basename(path))
    crashes = 0
    for n in range(int(count)):
        data = bytearray(original)
        if n % 3 == 0:
            data = data[:rng.randrange(1, len(data))]
        else:
            for _ in range(rng.randrange(1, 12)):
                data[rng.randrange(len(data))] = rng.randrange(256)
        open(scratch, "wb").write(data)
        try:
            code = subprocess.run([AMEDIA, "frames-out", scratch, "--size", "96x64", "--format", "rgb24", "--fps", "12"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60).returncode
        except subprocess.TimeoutExpired:
            code = -99
        if code not in (0, 1):
            crashes += 1
    os.remove(scratch)
    print("      %s damaged copies, %d crashes or hangs" % (count, crashes))
    return crashes == 0


def wav(path, rate):
    found, channels, data = read_wav(path)
    return found == int(rate) and channels == 1 and data.size > 0


def paced(least, most, *arguments):
    began = time.time()
    amedia(*arguments)
    spent = time.time() - began
    print("      took %.2f s (wanted %s to %s)" % (spent, least, most))
    return float(least) <= spent <= float(most)


def main():
    actions = {"psnr": psnr, "same": same, "swapped": swapped, "tone": tone, "voiced": voiced, "speech-tone": speech_tone, "speech-voice": speech_voice, "pattern": pattern, "recolour": recolour,
               "luma": luma, "randomly": randomly, "steady": steady, "turned": turned, "sound-like": sound_like, "sync": sync, "fuzz": fuzz, "wav": wav, "paced": paced}
    sys.exit(0 if actions[sys.argv[1]](*sys.argv[2:]) else 1)


if __name__ == "__main__":
    main()
