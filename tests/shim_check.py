import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SHIM = os.path.join(HERE, "..", "build", "shim")
AMEDIA = os.path.join(HERE, "..", "build", "ffamp")
REAL = {"ffmpeg": os.environ.get("FFMPEG", "ffmpeg"), "ffprobe": os.environ.get("FFPROBE", "ffprobe")}


def sound_call(tool, path, before, after, rate, channels):
    command = [tool, "-nostdin", "-v", "error"] + (["-ss", before] if before else []) + ["-i", path] + (["-ss", after] if after else [])
    command += ["-map", "0:a:0", "-vn", "-sn", "-dn", "-af", "aresample=async=1:first_pts=0", "-ar", str(rate), "-ac", str(channels), "-f", "f32le", "-"]
    return np.frombuffer(subprocess.run(command, capture_output=True).stdout, dtype="<f4").reshape(-1, channels).astype(np.float64)


def ratio(a, b):
    noise = ((a - b) ** 2).mean()
    return 200.0 if noise == 0 else 10 * np.log10(max((a ** 2).mean(), 1e-30) / noise)


def shifted(want, ours, most):
    best = (-999.0, 0)
    for lag in range(-most, most + 1):
        a = want[max(0, -lag):]
        b = ours[max(0, lag):]
        count = min(len(a), len(b))
        if count > 1000:
            best = max(best, (ratio(a[:count], b[:count]), lag))
    return best


def sound(path, rate, channels, judge):
    good = True
    for before, after in ((None, None), (None, "0.250000"), ("0.213015", "0.500000")):
        ours = sound_call(os.path.join(SHIM, "ffmpeg"), path, before, after, rate, channels)
        slack = 0
        if judge:
            want = sound_call(REAL["ffmpeg"], path, before, after, rate, channels)
            slack = rate // 1000 if before else 0
        else:
            first = sum(int(round(float(t or 0) * rate)) for t in (before, after))
            run = subprocess.run([AMEDIA, "pcm", path, "--rate", str(rate), "--channels", str(channels), "--first", str(first)], capture_output=True)
            want = np.frombuffer(run.stdout, dtype="<f4").reshape(-1, channels).astype(np.float64)
        level, lag = shifted(want, ours, slack) if len(ours) and len(want) else (0.0, 0)
        same = len(ours) > 0 and level > 40 and len(ours) - len(want) == lag
        good = good and same
        print(f"sound {os.path.basename(path)} seek {before}+{after}: {len(ours)} frames against {len(want)}, {level:.1f} dB with ours {lag} samples ahead (allowed {slack})")
    return good


def tone(path, wanted):
    run = subprocess.run([AMEDIA, "adecode", path], capture_output=True)
    pcm = np.frombuffer(run.stdout, dtype="<f4").reshape(-1, 2)[:, 0].astype(np.float64)
    spectrum = np.abs(np.fft.rfft(pcm[2000:2000 + 44100] * np.hanning(44100)))
    peak = float(np.argmax(spectrum))
    level = float(np.sqrt((pcm[2000:-2000] ** 2).mean()) * np.sqrt(2))
    print(f"tone {os.path.basename(path)}: strongest at {peak:.0f} Hz, amplitude {level:.3f}")
    return abs(peak - wanted) <= 1 and abs(level - 12000 / 32768) < 0.02


def times(path, judge):
    good = True
    for interval in ("%+#1", "0%", "0.7%", "1.31%"):
        for entry in ("frame", "packet"):
            if interval == "%+#1" and entry == "packet":
                continue
            arguments = ["-v", "error", "-select_streams", "v:0", "-read_intervals", interval, "-show_entries", entry + "=pts_time", "-of", "csv=p=0", path]
            ours = subprocess.run([os.path.join(SHIM, "ffprobe")] + arguments, capture_output=True, text=True).stdout.split()
            if judge:
                want = [line.strip().rstrip(",") for line in subprocess.run([REAL["ffprobe"]] + arguments, capture_output=True, text=True).stdout.split()]
                same = ours == want
            else:
                same = len(ours) > 0 and all(float(a) <= float(b) for a, b in zip(ours, ours[1:])) if entry == "frame" else len(ours) > 0
            good = good and same
            if not same:
                print(f"times {os.path.basename(path)} {interval} {entry}: {len(ours)} lines differ from the expected {len(want) if judge else 'order'}")
    return good


def main():
    mode = sys.argv[1]
    judge = "judge" in sys.argv[3:]
    if mode == "sound":
        return sound(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), "judge" in sys.argv[5:])
    if mode == "tone":
        return tone(sys.argv[2], float(sys.argv[3]))
    if mode == "times":
        return times(sys.argv[2], judge)
    return False


if __name__ == "__main__":
    sys.exit(0 if main() else 1)
