import os
import subprocess
import struct
import sys

import numpy

HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.environ.get("AMEDIA", os.path.join(HERE, "..", "build", "amedia"))


def samples(path):
    listing = subprocess.run([AMEDIA, "packets", path], capture_output=True, text=True).stdout.splitlines()
    track = None
    for line, following in zip(listing, listing[1:]):
        words = line.split()
        if words[0] == "track" and words[3] == "lpcm":
            track = words[1]
            shape = following.split()
            break
    if track is None:
        return None
    rate, channels, bits, floating, big = int(shape[1]), int(shape[3]), int(shape[6]), shape[8] == "1", shape[10] == "1"
    raw = subprocess.run([AMEDIA, "packets", path, "--track", track, "--raw"], capture_output=True).stdout
    order = ">" if big else "<"
    if floating:
        values = numpy.frombuffer(raw, dtype=order + ("f4" if bits == 32 else "f8")).astype(numpy.float64)
    elif bits == 8:
        values = (numpy.frombuffer(raw, dtype=numpy.uint8).astype(numpy.float64) - 128) / 128
    elif bits == 24:
        b = numpy.frombuffer(raw, dtype=numpy.uint8).reshape(-1, 3).astype(numpy.int32)
        low, middle, high = (b[:, 2], b[:, 1], b[:, 0]) if big else (b[:, 0], b[:, 1], b[:, 2])
        whole = low | (middle << 8) | (high << 16)
        values = numpy.where(whole >= 1 << 23, whole - (1 << 24), whole).astype(numpy.float64) / (1 << 23)
    else:
        values = numpy.frombuffer(raw, dtype=order + ("i2" if bits == 16 else "i4")).astype(numpy.float64) / (1 << (bits - 1))
    return rate, values.reshape(-1, channels)


def tone(path, wanted):
    found = samples(path)
    if found is None:
        print(f"tone {os.path.basename(path)}: no PCM track")
        return False
    rate, values = found
    left = values[:, int(numpy.argmax(numpy.max(numpy.abs(values), axis=0)))]
    spectrum = numpy.abs(numpy.fft.rfft(left * numpy.hanning(len(left))))
    peak = int(numpy.argmax(spectrum))
    position = peak + 0.5 * (spectrum[peak - 1] - spectrum[peak + 1]) / (spectrum[peak - 1] - 2 * spectrum[peak] + spectrum[peak + 1]) if 0 < peak < len(spectrum) - 1 else peak
    frequency = position * rate / len(left)
    level = float(numpy.max(numpy.abs(left)))
    print(f"tone {os.path.basename(path)}: {frequency:.1f} Hz, peak {level:.4f}")
    return abs(frequency - wanted) < 3 and abs(level - 0.125) < 0.004


def apple(path, scratch):
    found = samples(path)
    if found is None:
        print(f"apple {os.path.basename(path)}: no PCM track")
        return False
    rate, values = found
    if subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI32", path, scratch], capture_output=True).returncode != 0:
        print(f"apple {os.path.basename(path)}: afconvert cannot read it")
        return False
    converted = open(scratch, "rb").read()
    os.remove(scratch)
    at = 12
    described = stored = None
    while at + 8 <= len(converted):
        name, size = converted[at:at + 4], struct.unpack("<I", converted[at + 4:at + 8])[0]
        if name == b"fmt ":
            described = struct.unpack("<HHI", converted[at + 8:at + 16])
        elif name == b"data":
            stored = converted[at + 8:at + 8 + size]
        at += 8 + size + (size & 1)
    reference = numpy.frombuffer(stored, dtype="<i4").astype(numpy.float64).reshape(-1, described[1]) / (1 << 31)
    same_shape = described[2] == rate and reference.shape == values.shape
    worst = float(numpy.max(numpy.abs(reference - values))) if same_shape else 1.0
    print(f"apple {os.path.basename(path)}: frames {len(values)}, same shape {same_shape}, largest difference from Apple's reader {worst:.2e}")
    return same_shape and worst < 2e-7


if __name__ == "__main__":
    if sys.argv[1] == "tone":
        sys.exit(0 if tone(sys.argv[2], float(sys.argv[3])) else 1)
    sys.exit(0 if apple(sys.argv[2], sys.argv[3]) else 1)
