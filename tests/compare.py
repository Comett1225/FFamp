import os
import random
import subprocess
import sys
import time

import numpy

HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.environ.get("AMEDIA", os.path.join(HERE, "..", "build", "amedia"))
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
FFPROBE = os.environ.get("FFPROBE", "ffprobe")


def size_of(path):
    out = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height", "-of", "csv=p=0", path],
                         capture_output=True, text=True, check=True).stdout.split()[0].split(",")
    return int(out[0]), int(out[1])


def reference(path):
    return subprocess.Popen([FFMPEG, "-nostdin", "-v", "error", "-noautorotate", "-i", path, "-map", "0:v:0", "-fps_mode", "passthrough",
                             "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"], stdout=subprocess.PIPE)


def psnr(a, b):
    x = numpy.frombuffer(a, dtype=numpy.uint8).astype(numpy.float64)
    y = numpy.frombuffer(b, dtype=numpy.uint8).astype(numpy.float64)
    mse = numpy.mean((x - y) ** 2)
    return 99.0 if mse == 0 else 10 * numpy.log10(255 * 255 / mse)


def read(pipe, count):
    data = pipe.read(count)
    return data if len(data) == count else None


def decode(path):
    width, height = size_of(path)
    frame = width * height * 3 // 2
    ref = reference(path)
    ours = subprocess.Popen([AMEDIA, "decode", path], stdout=subprocess.PIPE)
    total = same = 0
    worst = 99.0
    while True:
        a = read(ref.stdout, frame)
        b = read(ours.stdout, frame)
        if a is None or b is None:
            extra = (a is not None) or (b is not None)
            break
        total += 1
        if a == b:
            same += 1
        else:
            worst = min(worst, psnr(a, b))
    ref.kill()
    ours.kill()
    print(f"decode {os.path.basename(path)}: frames {total}, identical {same}, lowest PSNR {worst:.2f} dB, count mismatch {extra}")
    return total > 0 and same == total and not extra


def seek(path, count, seed):
    width, height = size_of(path)
    frame = width * height * 3 // 2
    frames = int(subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0", "-count_packets", "-show_entries", "stream=nb_read_packets", "-of", "csv=p=0", path],
                                capture_output=True, text=True, check=True).stdout.split()[0].strip(","))
    rng = random.Random(seed)
    picks = [rng.randrange(frames) for _ in range(count - 2)] + [0, frames - 1]
    rng.shuffle(picks)
    wanted = set(picks)
    truth = {}
    ref = reference(path)
    for n in range(frames):
        data = read(ref.stdout, frame)
        if data is None:
            break
        if n in wanted:
            truth[n] = data
    ref.kill()
    run = subprocess.run([AMEDIA, "frames", path, "--list", ",".join(str(n) for n in picks)], capture_output=True)
    exact = 0
    worst = 99.0
    for i, n in enumerate(picks):
        got = run.stdout[i * frame:(i + 1) * frame]
        if len(got) == frame and n in truth and got == truth[n]:
            exact += 1
        elif len(got) == frame and n in truth:
            worst = min(worst, psnr(got, truth[n]))
        else:
            worst = 0.0
    summary = run.stderr.decode().strip().splitlines()[-1] if run.stderr else ""
    print(f"seek {os.path.basename(path)}: {exact}/{len(picks)} random frames exact, lowest PSNR {worst:.2f} dB; {summary}")
    return exact == len(picks)


def cli_seek(path, count, seed):
    width, height = size_of(path)
    info = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0", "-count_packets", "-show_entries", "stream=nb_read_packets,r_frame_rate", "-of", "csv=p=0", path],
                          capture_output=True, text=True, check=True).stdout.strip().strip(",").split(",")
    num, den = info[0].split("/")
    fps = int(num) / int(den)
    frames = int(info[1])
    rng = random.Random(seed)
    times = []
    for _ in range(count):
        n = rng.randrange(frames)
        start = time.time()
        subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-ss", f"{n / fps:.6f}", "-noautorotate", "-i", path, "-map", "0:v:0", "-frames:v", "1",
                        "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"], stdout=subprocess.DEVNULL, check=True)
        times.append((time.time() - start) * 1000)
    times.sort()
    print(f"ffmpeg CLI seek {os.path.basename(path)}: {count} seeks, mean {sum(times) / len(times):.1f} ms, median {times[len(times) // 2]:.1f} ms, max {times[-1]:.1f} ms")
    return True


def file_seek(path, index):
    width, height = size_of(path)
    frame = width * height * 3 // 2
    ref = reference(path)
    truth = None
    for n in range(index + 1):
        truth = read(ref.stdout, frame)
    ref.kill()
    got = subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-ss", f"{index / 30:.6f}", "-noautorotate", "-i", path, "-map", "0:v:0", "-frames:v", "1",
                          "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"], capture_output=True).stdout
    print(f"file seek {os.path.basename(path)}: frame {index} after a seek is {'identical' if got == truth else 'different'}")
    return truth is not None and got == truth


def audio(path, source, rate, channels):
    out = subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-i", path, "-map", "0:a:0", "-f", "s16le", "-ar", str(rate), "-ac", str(channels), "-"],
                         capture_output=True, check=True).stdout
    got = numpy.frombuffer(out, dtype=numpy.int16).astype(numpy.float64)
    want = numpy.fromfile(source, dtype=numpy.int16).astype(numpy.float64)
    n = min(len(got), len(want))
    edge = rate // 10 * channels
    a, b = got[edge:n - edge], want[edge:n - edge]
    noise = numpy.mean((a - b) ** 2)
    snr = 99.0 if noise == 0 else 10 * numpy.log10(numpy.mean(b ** 2) / noise)
    print(f"audio {os.path.basename(path)}: decoded {len(got) // channels} frames, source {len(want) // channels} frames, SNR {snr:.1f} dB")
    return abs(len(got) - len(want)) <= 2 * channels and snr > 20


if __name__ == "__main__":
    kind = sys.argv[1]
    if kind == "decode":
        ok = decode(sys.argv[2])
    elif kind == "seek":
        ok = seek(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]) if len(sys.argv) > 4 else 1)
    elif kind == "cliseek":
        ok = cli_seek(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]) if len(sys.argv) > 4 else 1)
    elif kind == "fileseek":
        ok = file_seek(sys.argv[2], int(sys.argv[3]))
    elif kind == "audio":
        ok = audio(sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]))
    else:
        ok = False
    sys.exit(0 if ok else 1)
