import os
import random
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")

STEREO = "aevalsrc=0.4*sin(2*PI*(300+900*t)*t)+0.05*(random(0)-0.5)|0.3*sin(2*PI*(500+400*t)*t)+0.05*(random(1)-0.5):s={rate}:d={seconds}"
MONO = "aevalsrc=0.4*sin(2*PI*(300+900*t)*t)+0.05*(random(0)-0.5):s={rate}:d={seconds}"

ENCODED = """
pcm_alaw 8000 2 - wav avi mkv mov caf aiff
pcm_mulaw 8000 2 - wav avi mkv mov caf aiff
pcm_s8 8000 2 - mov caf aiff
pcm_s64le 8000 2 - wav
adpcm_ima_wav 11025 2 - wav avi mkv mov
adpcm_ima_wav 11025 1 -block_size,256 wav
adpcm_ima_qt 11025 2 - mov caf aiff
adpcm_ms 11025 2 - wav avi mkv mov caf
adpcm_ms 11025 1 -block_size,256 wav
adpcm_yamaha 11025 2 - wav avi mkv
adpcm_g722 16000 1 - wav avi mkv
adpcm_g726 8000 1 -b:a,16k wav
adpcm_g726 8000 1 -b:a,24k wav
adpcm_g726 8000 1 -b:a,32k wav avi mkv
adpcm_g726 8000 1 -b:a,40k wav
flac 11025 2 - caf avi wav
tta 11025 2 -sample_fmt,s16 mkv
tta 11025 1 -sample_fmt,u8 mkv
tta 11025 2 -sample_fmt,s32 mkv
"""


def wave(path, tag, channels, rate, align, bits, data, extra=b""):
    form = struct.pack("<HHIIHH", tag, channels, rate, rate * align // max(1, align), align, bits)
    form += struct.pack("<H", len(extra)) + extra
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(form)) + form + b"data" + struct.pack("<I", len(data)) + data
    open(path, "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)


def random_payloads(folder, seed=11):
    rng = random.Random(seed)
    for bits in (2, 3, 5):
        for channels in (1, 2):
            align = 4 * channels + {2: 4, 3: 12, 5: 20}[bits] * channels * 16
            blocks = b""
            for _ in range(40):
                for _ in range(channels):
                    blocks += struct.pack("<hBB", rng.randrange(-30000, 30000), rng.randrange(0, 89), 0)
                blocks += bytes(rng.randrange(256) for _ in range(align - 4 * channels))
            wave(os.path.join(folder, f"ima{bits}_{channels}ch.wav"), 0x11, channels, 11025, align, bits, blocks)
    align = 6 * 7 + 6 * 100
    blocks = b""
    for _ in range(20):
        for _ in range(6):
            blocks += bytes([rng.randrange(7)]) + struct.pack("<hhh", rng.randrange(16, 2000), rng.randrange(-20000, 20000), rng.randrange(-20000, 20000))
            blocks += bytes(rng.randrange(256) for _ in range(100))
    wave(os.path.join(folder, "ms_6ch.wav"), 0x02, 6, 11025, align // 6, 4, blocks, struct.pack("<H", 1000))


def encoded(folder, seconds):
    for line in ENCODED.strip().splitlines():
        codec, rate, channels, extra, *containers = line.split()
        args = [] if extra == "-" else [a for a in extra.split(",")]
        name = codec + ("_" + args[-1] if args else "") + ("_mono" if channels == "1" and codec not in ("adpcm_g722", "adpcm_g726") else "")
        signal = (MONO if channels == "1" else STEREO).format(rate=rate, seconds=seconds)
        for container in containers:
            out = os.path.join(folder, f"{name}.{container}")
            subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i", signal, "-c:a", codec] + args + ["-fflags", "+bitexact", "-flags:a", "+bitexact", out], check=True)


if __name__ == "__main__":
    folder = sys.argv[2] if len(sys.argv) > 2 else HERE
    if sys.argv[1] in ("random", "all"):
        random_payloads(folder)
    if sys.argv[1] in ("encoded", "all"):
        encoded(folder, sys.argv[3] if len(sys.argv) > 3 else "0.25")
