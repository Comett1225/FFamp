import json
import os
import subprocess
import sys

import numpy

AMEDIA = "../build/amedia"
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
VLC = "/Applications/VLC.app/Contents/MacOS/VLC"
PACKET = 188


def hollow(source, target):
    data = open(source, "rb").read()
    counts = {}
    for at in range(0, len(data) - PACKET + 1, PACKET):
        pid = ((data[at + 1] & 0x1F) << 8) | data[at + 2]
        counts[pid] = counts.get(pid, 0) + 1
    busiest = max((pid for pid in counts if pid not in (0, 0x1FFF)), key=lambda pid: counts[pid])
    out = bytearray()
    counter = None
    added = 0
    for at in range(0, len(data) - PACKET + 1, PACKET):
        packet = bytearray(data[at:at + PACKET])
        pid = ((packet[1] & 0x1F) << 8) | packet[2]
        if pid == busiest and packet[3] & 0x10:
            if counter is not None and packet[1] & 0x40:
                for _ in range(4):
                    counter = (counter + 1) & 15
                    out += bytes([0x47, pid >> 8, pid & 0xFF, 0x30 | counter, 183, 0]) + b"\xff" * 182
                    added += 1
            counter = (packet[3] & 15) if counter is None else (counter + 1) & 15
            packet[3] = (packet[3] & 0xF0) | counter
        out += packet
    open(target, "wb").write(out)
    print(f"{os.path.basename(target)}: {added} empty packets added on PID {busiest}")
    return added > 0


def apv(encoded, source, width, height, floor, with_ffmpeg):
    frame = width * height * 4
    got = subprocess.run([AMEDIA, "decode", encoded], capture_output=True).stdout
    want = numpy.fromfile(source, dtype=numpy.uint8)
    small = width * height * 3 // 2
    count = len(got) // frame
    if count == 0 or len(got) != count * frame or len(want) < count * small:
        print(f"{os.path.basename(encoded)}: {len(got)} decoded bytes for frames of {frame}")
        return False
    ours = numpy.frombuffer(got, dtype="<u2").reshape(count, -1)
    luma = ours[:, :width * height].astype(numpy.float64)
    truth = want[:count * small].reshape(count, small)[:, :width * height].astype(numpy.float64) * 4
    psnr = 10 * numpy.log10(1023 * 1023 / max(numpy.mean((luma - truth) ** 2), 1e-9))
    chroma = ours[:, width * height:].astype(numpy.float64).mean() / 4
    wanted = want[:count * small].reshape(count, small)[:, width * height:].astype(numpy.float64).mean()
    same = "not asked"
    if with_ffmpeg:
        judge = subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-i", encoded, "-fps_mode", "passthrough", "-pix_fmt", "yuv422p10le", "-f", "rawvideo", "-"], capture_output=True).stdout
        same = judge == got
    print(f"{os.path.basename(encoded)}: {count} frames, luma PSNR {psnr:.1f} dB, chroma level {chroma:.1f} against {wanted:.1f}, same as ffmpeg's decode: {same}")
    return psnr >= floor and abs(chroma - wanted) < 1.5 and same is not False


def vlc(path, demux, wanted):
    try:
        run = subprocess.run([VLC, "-I", "dummy", "--play-and-exit", "--vout=dummy", "--aout=dummy", "--no-video-title-show", "--verbose=2", "--stop-time=2", path],
                             capture_output=True, timeout=60)
    except subprocess.TimeoutExpired:
        print(f"{os.path.basename(path)}: VLC did not finish")
        return False
    log = (run.stdout + run.stderr).decode(errors="replace")
    errors = [line for line in log.splitlines() if " error: " in line and "Cannot create vout" not in line]
    found = f'using demux module "{demux}"' in log
    kinds = [kind for kind in wanted if f"using {kind} decoder module" not in log]
    print(f"{os.path.basename(path)}: VLC exit {run.returncode}, demux {demux} {'used' if found else 'not used'}, decoders missing {kinds}, {len(errors)} error line(s)")
    for line in errors[:3]:
        print("  " + line)
    return run.returncode == 0 and found and not kinds and not errors


def mkv(path):
    run = subprocess.run(["mkvmerge", "-J", path], capture_output=True)
    try:
        report = json.loads(run.stdout)
    except ValueError:
        print(f"{path}: mkvmerge gave no report")
        return False
    ok = run.returncode == 0 and not report.get("errors") and not report.get("warnings") and report.get("container", {}).get("recognized") and report.get("tracks")
    if not ok:
        print(f"{path}: mkvmerge exit {run.returncode}, errors {report.get('errors')}, warnings {report.get('warnings')}")
    return bool(ok)


if __name__ == "__main__":
    kind = sys.argv[1]
    a = sys.argv[2:]
    if kind == "hollow":
        ok = hollow(a[0], a[1])
    elif kind == "apv":
        ok = apv(a[0], a[1], int(a[2]), int(a[3]), float(a[4]), "ffmpeg" in a[5:])
    elif kind == "vlc":
        ok = vlc(a[0], a[1], a[2:])
    elif kind == "mkv":
        ok = all([mkv(path) for path in a])
    else:
        ok = False
    sys.exit(0 if ok else 1)
