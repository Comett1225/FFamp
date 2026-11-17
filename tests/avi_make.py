import os
import struct
import subprocess
import sys

FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
SIZE = "320x240"
ODD = "322x242"


def ff(*args):
    return subprocess.run([FFMPEG, "-nostdin", "-hide_banner", "-v", "error", "-y", *args]).returncode == 0


def picture(size=SIZE, rate="25", seconds="2", extra=""):
    return ["-f", "lavfi", "-t", seconds, "-i", f"testsrc2=size={size}:rate={rate}{extra}"]


def sound(rate=44100, seconds="2"):
    return ["-f", "lavfi", "-t", seconds, "-i", f"sine=frequency=440:sample_rate={rate}:beep_factor=3"]


VIDEO = {
    "mjpeg": ["-c:v", "mjpeg", "-q:v", "3", "-pix_fmt", "yuvj420p"],
    "mjpeg422": ["-c:v", "mjpeg", "-q:v", "3", "-pix_fmt", "yuvj422p"],
    "xvid": ["-c:v", "mpeg4", "-vtag", "XVID", "-q:v", "4", "-bf", "2", "-g", "12"],
    "divx": ["-c:v", "mpeg4", "-vtag", "DIVX", "-q:v", "4", "-g", "12"],
    "dx50": ["-c:v", "mpeg4", "-vtag", "DX50", "-q:v", "4", "-bf", "1", "-g", "12"],
    "fmp4": ["-c:v", "mpeg4", "-vtag", "FMP4", "-q:v", "4", "-g", "12"],
    "div3": ["-c:v", "msmpeg4", "-vtag", "DIV3", "-q:v", "4", "-g", "12"],
    "mp42": ["-c:v", "msmpeg4v2", "-q:v", "4", "-g", "12"],
    "h264": ["-c:v", "libx264", "-preset", "veryfast", "-bf", "2", "-g", "12", "-pix_fmt", "yuv420p"],
    "mpeg2": ["-c:v", "mpeg2video", "-q:v", "4", "-bf", "2", "-g", "12"],
    "mpeg1": ["-c:v", "mpeg1video", "-q:v", "4", "-bf", "1", "-g", "12"],
    "bgr24": ["-c:v", "rawvideo", "-pix_fmt", "bgr24"],
    "bgra": ["-c:v", "rawvideo", "-pix_fmt", "bgra"],
    "rgb555": ["-c:v", "rawvideo", "-pix_fmt", "rgb555le"],
    "pal8": ["-c:v", "rawvideo", "-pix_fmt", "pal8"],
    "yuy2": ["-c:v", "rawvideo", "-pix_fmt", "yuyv422", "-vtag", "YUY2"],
    "uyvy": ["-c:v", "rawvideo", "-pix_fmt", "uyvy422", "-vtag", "UYVY"],
    "i420": ["-c:v", "rawvideo", "-pix_fmt", "yuv420p", "-vtag", "I420"],
    "yv12": ["-c:v", "rawvideo", "-pix_fmt", "yuv420p", "-vtag", "YV12"],
    "gray": ["-c:v", "rawvideo", "-pix_fmt", "gray", "-vtag", "Y800"],
    "cinepak": ["-c:v", "cinepak", "-g", "10"],
    "huffyuv": ["-c:v", "huffyuv", "-pix_fmt", "yuv422p"],
    "utvideo": ["-c:v", "utvideo", "-pix_fmt", "yuv420p"],
    "magicyuv": ["-c:v", "magicyuv", "-pix_fmt", "yuv422p"],
    "dv": ["-c:v", "dvvideo", "-pix_fmt", "yuv420p"],
}

AUDIO = {
    "pcm": ["-c:a", "pcm_s16le", "-ac", "2"],
    "u8": ["-c:a", "pcm_u8", "-ac", "1"],
    "s24": ["-c:a", "pcm_s24le", "-ac", "2"],
    "f32": ["-c:a", "pcm_f32le", "-ac", "2"],
    "alaw": ["-c:a", "pcm_alaw", "-ac", "1", "-ar", "8000"],
    "mulaw": ["-c:a", "pcm_mulaw", "-ac", "1", "-ar", "8000"],
    "ima": ["-c:a", "adpcm_ima_wav", "-ac", "2"],
    "msadpcm": ["-c:a", "adpcm_ms", "-ac", "2"],
    "mp3": ["-c:a", "libmp3lame", "-b:a", "128k", "-ac", "2"],
    "mp3vbr": ["-c:a", "libmp3lame", "-q:a", "4", "-ac", "2"],
    "mp2": ["-c:a", "mp2", "-b:a", "192k", "-ac", "2"],
    "ac3": ["-c:a", "ac3", "-b:a", "192k", "-ac", "2", "-ar", "48000"],
    "aac": ["-c:a", "aac", "-b:a", "128k", "-ac", "2"],
    "wma": ["-c:a", "wmav2", "-b:a", "128k", "-ac", "2"],
    "pcm48": ["-c:a", "pcm_s16le", "-ac", "2", "-ar", "48000"],
    "eac3": ["-c:a", "eac3", "-b:a", "192k", "-ac", "2", "-ar", "48000"],
}

ENCODES = [
    ("mjpeg_pcm", "mjpeg", "pcm", {}),
    ("mjpeg422_u8", "mjpeg422", "u8", {}),
    ("xvid_mp3", "xvid", "mp3", {}),
    ("divx_mp3vbr", "divx", "mp3vbr", {}),
    ("dx50_ac3", "dx50", "ac3", {}),
    ("fmp4_mp2", "fmp4", "mp2", {}),
    ("div3_mp3", "div3", "mp3", {}),
    ("mp42_s24", "mp42", "s24", {}),
    ("h264_aac", "h264", "aac", {}),
    ("h264_ntsc_ac3", "h264", "ac3", {"rate": "30000/1001"}),
    ("mpeg2_mp2", "mpeg2", "mp2", {}),
    ("mpeg1_pcm", "mpeg1", "pcm", {"rate": "24000/1001"}),
    ("bgr24_pcm", "bgr24", "pcm", {}),
    ("bgr24_odd", "bgr24", None, {"size": ODD}),
    ("bgra_f32", "bgra", "f32", {}),
    ("rgb555_alaw", "rgb555", "alaw", {}),
    ("pal8_mulaw", "pal8", "mulaw", {}),
    ("yuy2_ima", "yuy2", "ima", {}),
    ("uyvy_msadpcm", "uyvy", "msadpcm", {}),
    ("i420_pcm", "i420", "pcm", {}),
    ("yv12_pcm", "yv12", "pcm", {}),
    ("gray_pcm", "gray", "pcm", {}),
    ("cinepak_ima", "cinepak", "ima", {}),
    ("cinepak_small", "cinepak", None, {"size": "176x120"}),
    ("mjpeg_wma", "mjpeg", "wma", {}),
    ("huffyuv_pcm", "huffyuv", "pcm", {}),
    ("utvideo_pcm", "utvideo", "pcm", {}),
    ("magicyuv_pcm", "magicyuv", "pcm", {}),
    ("dv_pcm48", "dv", "pcm48", {"size": "720x576"}),
    ("mjpeg_eac3", "mjpeg", "eac3", {}),
]


def encode(folder, name, video, audio, options):
    target = os.path.join(folder, f"avi_{name}.avi")
    if os.path.exists(target):
        return target
    args = picture(options.get("size", SIZE), options.get("rate", "25"))
    if audio:
        args += sound(48000 if audio in ("ac3", "eac3", "pcm48") else 44100)
    args += ["-map", "0:v"] + (["-map", "1:a"] if audio else [])
    args += VIDEO[video] + (AUDIO[audio] if audio else [])
    return target if ff(*args, target) else None


def chunk(name, body):
    return name + struct.pack("<I", len(body)) + body + (b"\x00" if len(body) & 1 else b"")


def walk(data, at, end):
    while at + 8 <= end:
        name = data[at:at + 4]
        size = struct.unpack("<I", data[at + 4:at + 8])[0]
        yield name, at, size
        at += 8 + size + (size & 1)


class Avi:
    def __init__(self, path):
        data = open(path, "rb").read()
        self.hdrl = None
        self.entries = []
        keys = {}
        at = 0
        movies = []
        while at + 12 <= len(data) and data[at:at + 4] == b"RIFF":
            size = struct.unpack("<I", data[at + 4:at + 8])[0]
            end = min(len(data), at + 8 + size)
            for name, place, length in walk(data, at + 12, end):
                kind = data[place + 8:place + 12]
                if name == b"LIST" and kind == b"hdrl" and self.hdrl is None:
                    self.hdrl = bytearray(data[place + 12:place + 8 + length])
                elif name == b"LIST" and kind == b"movi":
                    movies.append((place + 12, place + 8 + length))
            at = end + (end & 1)
        for begin, end in movies:
            for name, place, length in walk(data, begin, end):
                if name[:2] == b"ix" or name[2:] == b"ix":
                    count = struct.unpack("<I", data[place + 12:place + 16])[0]
                    base = struct.unpack("<Q", data[place + 20:place + 28])[0]
                    for i in range(count):
                        offset, size = struct.unpack("<II", data[place + 32 + 8 * i:place + 40 + 8 * i])
                        keys[base + offset - 8] = not size & 0x80000000
        for begin, end in movies:
            for name, place, length in walk(data, begin, end):
                if name == b"LIST":
                    for inner, spot, size in walk(data, place + 12, place + 8 + length):
                        self.entries.append([inner, data[spot + 8:spot + 8 + size], keys.get(spot, True)])
                elif name[:2] != b"ix" and name[2:] != b"ix" and name != b"JUNK":
                    self.entries.append([name, data[place + 8:place + 8 + length], keys.get(place, True)])
        hdrl = bytes(self.hdrl)
        self.hdrl = hdrl.replace(b"indx", b"JUNK")

    def write(self, path, index=True, pad=True, junk=False, garbage=False, absolute=False, rec=False, order=None, cut=None, keep=None, flags=None):
        entries = self.entries if order is None else order(self.entries)
        movi = bytearray(b"movi")
        table = bytearray()
        head = chunk(b"LIST", b"hdrl" + self.hdrl)
        start = 12 + len(head) + 8 if absolute else 0

        def stored(name, body):
            return name + struct.pack("<I", len(body)) + body + (b"\x00" if pad and len(body) & 1 else b"")

        def listed(entry, spot):
            name, body, key = entry
            return struct.pack("<4sIII", name, (0x10 if key else 0) if flags is None else flags, start + spot, len(body))

        n = 0
        while n < len(entries):
            if junk and n % 5 == 2:
                movi += chunk(b"JUNK", b"\x00" * (n % 7 + 3))
            if garbage and n % 9 == 4:
                movi += b"\x13\x37garbage" * (n % 3 + 1)
            group = entries[n:n + 2] if rec else entries[n:n + 1]
            if rec:
                inner = bytearray(b"rec ")
                for entry in group:
                    table += listed(entry, len(movi) + 8 + len(inner))
                    inner += stored(*entry[:2])
                movi += chunk(b"LIST", bytes(inner))
            else:
                table += listed(group[0], len(movi))
                movi += stored(*group[0][:2])
            n += len(group)
        if keep is not None:
            table = table[:16 * int(len(table) // 16 * keep)]
        body = b"AVI " + head + chunk(b"LIST", bytes(movi)) + (chunk(b"idx1", bytes(table)) if index else b"")
        out = b"RIFF" + struct.pack("<I", len(body)) + body
        if cut:
            out = out[:int(len(out) * cut)]
        open(path, "wb").write(out)


def split(entries):
    return [e for e in entries if e[0][2:] == b"dc" or e[0][2:] == b"db"] + [e for e in entries if e[0][2:] == b"wb"]


def dropped(entries):
    out = []
    for n, e in enumerate(entries):
        out.append(e)
        if e[0][2:] in (b"dc", b"db") and n % 7 == 3:
            out.append([e[0], b"", False])
    return out


def strh(path, target, stream, scale=None, rate=None, start=None):
    data = bytearray(open(path, "rb").read())
    at = -1
    for _ in range(stream + 1):
        at = data.index(b"strh", at + 1)
    if scale is not None:
        data[at + 28:at + 32] = struct.pack("<I", scale)
    if rate is not None:
        data[at + 32:at + 36] = struct.pack("<I", rate)
    if start is not None:
        data[at + 36:at + 40] = struct.pack("<I", start)
    open(target, "wb").write(data)


def structures(folder):
    made = []
    base = os.path.join(folder, "avi_xvid_mp3.avi")
    avi = Avi(base)
    variants = {
        "idx1": {},
        "noindex": {"index": False},
        "shortidx": {"keep": 0.4},
        "absolute": {"absolute": True},
        "unpadded": {"index": False, "pad": False},
        "unpadded_idx1": {"pad": False},
        "junk": {"index": False, "junk": True},
        "rec": {"rec": True},
        "garbage": {"index": False, "garbage": True},
        "cut": {"index": False, "cut": 0.73},
        "split": {"order": split},
        "split_noindex": {"order": split, "index": False},
        "dropped": {"order": dropped},
        "dropped_noindex": {"order": dropped, "index": False},
        "nokeys": {"flags": 0},
    }
    for name, options in variants.items():
        target = os.path.join(folder, f"avs_{name}.avi")
        if not os.path.exists(target):
            avi.write(target, **options)
        made.append(target)
    for source, name, options in (("avi_h264_aac.avi", "h264_noindex", {"index": False}), ("avi_cinepak_ima.avi", "cinepak_noindex", {"index": False}),
                                  ("avi_mjpeg_pcm.avi", "mjpeg_split", {"order": split}), ("avi_div3_mp3.avi", "div3_nokeys", {"flags": 0}),
                                  ("avi_bgr24_odd.avi", "bgr24_unpadded", {"index": False, "pad": False})):
        target = os.path.join(folder, f"avs_{name}.avi")
        if os.path.exists(os.path.join(folder, source)) and not os.path.exists(target):
            Avi(os.path.join(folder, source)).write(target, **options)
        made.append(target)
    target = os.path.join(folder, "avs_scale.avi")
    strh(os.path.join(folder, "avi_mjpeg_pcm.avi"), target, 0, scale=40, rate=1000)
    made.append(target)
    target = os.path.join(folder, "avs_audio_start.avi")
    strh(os.path.join(folder, "avi_mjpeg_pcm.avi"), target, 1, start=11025)
    made.append(target)
    return made


def special(folder):
    made = []
    target = os.path.join(folder, "avi_h264_avc1.avi")
    mp4 = os.path.join(folder, "avc1_source.mp4")
    if not os.path.exists(target):
        ff(*picture(), *sound(), "-map", "0:v", "-map", "1:a", *VIDEO["h264"], *AUDIO["aac"], mp4)
        ff("-i", mp4, "-c", "copy", "-bsf:v", "h264_mp4toannexb,h264_metadata", target + ".annexb.avi")
        ff("-i", mp4, "-c", "copy", target)
        if os.path.exists(target + ".annexb.avi"):
            os.remove(target + ".annexb.avi")
    made.append(target)
    target = os.path.join(folder, "avi_mjpeg_adts.avi")
    adts = os.path.join(folder, "adts_source.aac")
    if not os.path.exists(target):
        ff(*sound(), *AUDIO["aac"], adts)
        ff(*picture(), "-i", adts, "-map", "0:v", "-map", "1:a", *VIDEO["mjpeg"], "-c:a", "copy", "-tag:a", "0x1610", target)
    made.append(target)
    target = os.path.join(folder, "avi_vfr_dropped.avi")
    if not os.path.exists(target):
        ff(*picture(), *sound(), "-map", "0:v", "-map", "1:a", "-vf", "select='not(eq(mod(n\\,6)\\,4))'", "-fps_mode", "passthrough", *VIDEO["xvid"], *AUDIO["mp3"], target)
    made.append(target)
    return made


PAL_SHUFFLE = [[0, 36, 72, 26, 62, 98, 16, 52, 88], [6, 42, 78, 32, 68, 104, 22, 58, 94], [12, 48, 84, 2, 38, 74, 28, 64, 100],
               [18, 54, 90, 8, 44, 80, 34, 70, 106], [24, 60, 96, 14, 50, 86, 4, 40, 76], [30, 66, 102, 20, 56, 92, 10, 46, 82]]


def twelve_bit(frame, seed):
    out = bytearray(frame)
    for c in range(12):
        for at in (c * 12000 + 80 * 6 + 3, c * 12000 + 80 * 6 + 80 * 16 * 3 + 3):
            if out[at] == 0x50:
                out[at + 1] = (out[at + 1] & 0xC0) | 16
                out[at + 4] = (out[at + 4] & 0xC0) | (2 << 3) | 1
    state = seed
    for i in range(6):
        for j in range(9):
            block = i * 12000 + 80 * 6 + j * 16 * 80
            for d in range(8, 80, 3):
                state = (state * 1103515245 + 12345) & 0x7FFFFFFF
                left, right = (state >> 4) & 0xFFF, (state >> 16) & 0xFFF
                out[block + d] = left >> 4
                out[block + d + 1] = right >> 4
                out[block + d + 2] = ((left & 15) << 4) | (right & 15)
    return bytes(out)


def tape(folder):
    made = []
    raw = os.path.join(folder, "type1_source.dv")
    if not os.path.exists(raw) and not ff(*picture("720x576"), *sound(48000), "-map", "0:v", "-map", "1:a", "-c:v", "dvvideo", "-pix_fmt", "yuv420p",
                                          "-c:a", "pcm_s16le", "-ac", "2", "-f", "dv", raw):
        return made
    data = open(raw, "rb").read()
    source = [data[at:at + 144000] for at in range(0, len(data) - 143999, 144000)]
    for name, frames in (("avi_dv_type1.avi", source), ("avi_dv_type1_12bit.avi", [twelve_bit(f, n + 1) for n, f in enumerate(source)])):
        target = os.path.join(folder, name)
        made.append(target)
        if os.path.exists(target):
            continue
        main = struct.pack("<14I", 40000, 0, 0, 0x10, len(frames), 0, 1, 0, 720, 576, 0, 0, 0, 0)
        stream = b"iavsdvsd" + struct.pack("<10I", 0, 0, 0, 1, 25, 0, len(frames), 144000, 0xFFFFFFFF, 0) + struct.pack("<HHHH", 0, 0, 720, 576)
        header = b"hdrl" + chunk(b"avih", main) + chunk(b"LIST", b"strl" + chunk(b"strh", stream) + chunk(b"strf", b"\x00" * 32))
        movie = b"movi"
        index = b""
        for frame in frames:
            index += b"00__" + struct.pack("<3I", 0x10, len(movie), len(frame))
            movie += chunk(b"00__", frame)
        body = b"AVI " + chunk(b"LIST", header) + chunk(b"LIST", movie) + chunk(b"idx1", index)
        open(target, "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)
    return made


def big(folder, gigabytes):
    target = os.path.join(folder, f"avb_{gigabytes}g.avi")
    if not os.path.exists(target):
        seconds = str(int(gigabytes * (1 << 30) / (1280 * 720 * 2 * 25 + 192000)) + 1)
        ff("-f", "lavfi", "-t", seconds, "-i", "testsrc2=size=1280x720:rate=25", "-f", "lavfi", "-t", seconds, "-i", "sine=frequency=440:sample_rate=48000",
           "-map", "0:v", "-map", "1:a", "-c:v", "rawvideo", "-pix_fmt", "uyvy422", "-vtag", "UYVY", "-c:a", "pcm_s16le", target)
    flat = os.path.join(folder, f"avb_{gigabytes}g_idx1.avi")
    if not os.path.exists(flat):
        data = open(target, "rb")
        with open(flat, "wb") as out:
            while True:
                block = data.read(64 << 20)
                if not block:
                    break
                out.write(block.replace(b"indx", b"JUNK"))
    return [target, flat]


if __name__ == "__main__":
    folder = sys.argv[1]
    os.makedirs(folder, exist_ok=True)
    made = [encode(folder, *item) for item in ENCODES]
    made += special(folder)
    made += tape(folder)
    made += structures(folder)
    if len(sys.argv) > 2:
        made += big(folder, float(sys.argv[2]))
    for path in made:
        print(path if path and os.path.exists(path) else f"missing {path}")
