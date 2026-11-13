import json
import os
import struct
import subprocess
import sys
import tempfile

import numpy

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "writers")
QUIET = ["ffmpeg", "-nostdin", "-hide_banner", "-v", "error", "-y"]
PICTURE = ["-f", "lavfi", "-i", "testsrc2=size=160x90:rate=10", "-frames:v", "12", "-pix_fmt", "yuv420p", "-colorspace", "bt709", "-color_primaries", "bt709",
           "-color_trc", "bt709"]
SOUND = ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-t", "0.6", "-ac", "2"]
CODES = {"av1": (b"av01", b"av1C"), "vp9": (b"vp09", b"vpcC"), "vp8": (b"vp08", b"vpcC"), "opus": (b"Opus", b"OpHd"), "flac": (b"fLaC", b"fLaC"), "alac": (b"alac", b"alac"),
         "prores": (b"apcn", b"\0\0\0\0"), "h264": (b"avc1", b"avcC"), "hevc": (b"hvc1", b"hvcC"), "aac": (b"mp4a", b"ASC ")}


def dump(text):
    out = bytearray()
    for line in text.strip().splitlines():
        out += bytes.fromhex(line.split(": ", 1)[1][:40].replace(" ", ""))
    return bytes(out)


def track(kind, codec, config_type, timescale, config, packets, width=0, height=0, rate=0, channels=0, bits=0, flags=0, skip=0):
    head = struct.pack(">B4s4sIHHIHBBqqBBBBI", kind, codec, config_type, timescale, width, height, rate, channels, bits, flags, skip, 0, 1, 1, 1, 0, len(config)) + config
    body = b"".join(struct.pack(">qqIBI", pts, dts, duration, key, len(data)) + data for pts, dts, duration, key, data in packets)
    return head + struct.pack(">I", len(packets)) + body


def save(name, *tracks):
    with open(os.path.join(OUT, name + ".pkts"), "wb") as f:
        f.write(b"APKT" + struct.pack(">I", len(tracks)) + b"".join(tracks))


def from_ffmpeg(name, arguments, suffix):
    with tempfile.TemporaryDirectory() as folder:
        path = os.path.join(folder, "clip" + suffix)
        subprocess.run(QUIET + arguments + [path], check=True)
        probe = json.loads(subprocess.run(["ffprobe", "-v", "error", "-show_streams", "-show_packets", "-show_data", "-of", "json", path], capture_output=True, check=True).stdout)
    stream = probe["streams"][0]
    codec, config_type = CODES[stream["codec_name"]]
    config = dump(stream["extradata"]) if "extradata" in stream else b""
    if stream["codec_name"] == "alac":
        config = config[-24:]
    timescale = int(stream["time_base"].split("/")[1])
    base = min(int(p["dts"]) for p in probe["packets"])
    packets = [(int(p["pts"]) - base, int(p["dts"]) - base, int(p["duration"]), 1 if "K" in p["flags"] else 0, dump(p["data"])) for p in probe["packets"]]
    if stream["codec_type"] == "video":
        save(name, track(0, codec, config_type, timescale, config, packets, width=stream["width"], height=stream["height"]))
    else:
        skip = struct.unpack("<H", config[10:12])[0] if stream["codec_name"] == "opus" else 0
        bits = int(stream.get("bits_per_raw_sample") or 0)
        save(name, track(1, codec, config_type, timescale, config, packets, rate=int(stream["sample_rate"]), channels=stream["channels"], bits=bits, skip=skip))


def pcm(name, kind, bits, flags, rate=48000, channels=2, frames=9000):
    t = numpy.arange(frames) / rate
    wave = numpy.stack([0.5 * numpy.sin(2 * numpy.pi * 440 * t + c) for c in range(channels)], 1)
    data = (wave.astype(kind) if flags & 1 else numpy.round(wave * (2 ** (bits - 1) - 1)).astype(kind)).tobytes()
    if bits == 24:
        raw = numpy.frombuffer(numpy.round(wave * (2 ** 23 - 1)).astype("<i4").tobytes(), dtype=numpy.uint8).reshape(-1, 4)[:, :3]
        data = raw.tobytes()
    size = channels * bits // 8
    packets = [(at, at, min(4096, frames - at), 1, data[at * size:(at + 4096) * size]) for at in range(0, frames, 4096)]
    save(name, track(1, b"lpcm", b"\0\0\0\0", rate, b"", packets, rate=rate, channels=channels, bits=bits, flags=flags))


os.makedirs(OUT, exist_ok=True)
from_ffmpeg("av1", PICTURE + ["-c:v", "libsvtav1", "-preset", "8", "-svtav1-params", "lp=1", "-g", "6"], ".mp4")
from_ffmpeg("vp9", PICTURE + ["-c:v", "libvpx-vp9", "-b:v", "150k", "-g", "6"], ".mp4")
from_ffmpeg("vp8", PICTURE + ["-c:v", "libvpx", "-b:v", "150k", "-g", "6"], ".ivf")
from_ffmpeg("prores", PICTURE[:4] + ["-frames:v", "4", "-c:v", "prores_ks", "-profile:v", "2", "-pix_fmt", "yuv422p10le"], ".mov")
from_ffmpeg("h264", PICTURE + ["-c:v", "libx264", "-bf", "2", "-g", "6"], ".mp4")
from_ffmpeg("hevc", PICTURE + ["-c:v", "libx265", "-x265-params", "log-level=error", "-tag:v", "hvc1", "-g", "6"], ".mp4")
from_ffmpeg("opus", SOUND + ["-c:a", "libopus", "-b:a", "64k"], ".mp4")
from_ffmpeg("flac", SOUND + ["-c:a", "flac", "-sample_fmt", "s16"], ".mp4")
from_ffmpeg("alac", SOUND + ["-c:a", "alac", "-sample_fmt", "s16p"], ".m4a")
from_ffmpeg("aac", SOUND + ["-c:a", "aac", "-b:a", "96k"], ".m4a")
pcm("pcm_s16", "<i2", 16, 0)
pcm("pcm_s24", "<i4", 24, 0)
pcm("pcm_f32", "<f4", 32, 1)
pcm("pcm_s16be", ">i2", 16, 2)
pcm("pcm_f64_6ch", "<f8", 64, 1, 44100, 6, 2000)
for name in sorted(os.listdir(OUT)):
    print(name, os.path.getsize(os.path.join(OUT, name)))
