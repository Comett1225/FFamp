import os, subprocess, sys, time
FF = os.environ["FFMPEG"]
A = os.environ["AMEDIA"]
D = sys.argv[1]
os.makedirs(D, exist_ok=True)
F = [FF, "-nostdin", "-v", "error", "-y"]
src6 = "aevalsrc=0.3*sin(2*PI*(200+400*t)*t)|0.3*sin(2*PI*(300+200*t)*t)|0.2*sin(2*PI*500*t)|0.3*sin(2*PI*60*t)|0.2*sin(2*PI*(700+50*t)*t)|0.2*sin(2*PI*(900+30*t)*t):s=48000:d=120:c=5.1"
src2 = "aevalsrc=0.3*sin(2*PI*(200+400*t)*t)|0.3*sin(2*PI*(300+200*t)*t):s=44100:d=120"
src1 = "aevalsrc=0.3*sin(2*PI*(200+400*t)*t)+0.2*sin(2*PI*(900+30*t)*t):s=8000:d=120"
def make(name, args):
    path = os.path.join(D, name)
    if not os.path.exists(path):
        subprocess.run(F + args + [path], check=True)
    return path
def repeat(name, source, times, header=0):
    path = os.path.join(D, name)
    data = open(source, "rb").read()
    open(path, "wb").write(data + data[header:] * (times - 1))
    return path
files = [
    ("AAC-LC 5.1 384 kbit/s", make("lc51.m4a", ["-f", "lavfi", "-i", src6, "-c:a", "aac", "-b:a", "384k"])),
    ("AAC-LC stereo 128 kbit/s", make("lc2.m4a", ["-f", "lavfi", "-i", src2, "-c:a", "aac", "-b:a", "128k"])),
    ("HE-AAC stereo 48 kbit/s", repeat("he.aac", "fixtures/aac/he_stereo_44k.aac", 75)),
    ("HE-AAC v2 32 kbit/s", repeat("hev2.aac", "fixtures/aac/he_v2_44k.aac", 75)),
    ("AC-3 5.1 448 kbit/s", make("ac3.ac3", ["-f", "lavfi", "-i", src6, "-c:a", "ac3", "-b:a", "448k"])),
    ("E-AC-3 5.1 448 kbit/s", make("eac3.eac3", ["-f", "lavfi", "-i", src6, "-c:a", "eac3", "-b:a", "448k"])),
    ("AMR-NB 12.2 kbit/s", make("nb.amr", ["-f", "lavfi", "-i", src1, "-c:a", "libopencore_amrnb", "-b:a", "12200"])),
    ("AMR-WB 23.85 kbit/s", repeat("wb.amr", "fixtures/formats/amr_wb.amr", 80, 9)),
]
def best(cmd, runs=5):
    t = []
    for _ in range(runs):
        s = time.perf_counter()
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        t.append(time.perf_counter() - s)
    return min(t)
for label, path in files:
    ours = best([A, "adecode", path])
    theirs = best([FF, "-nostdin", "-v", "error", "-threads", "1", "-i", path, "-map", "0:a:0", "-f", "f32le", "-"])
    extra = ""
    if "AMR" in label:
        lib = "libopencore_amrnb" if "NB" in label else "libopencore_amrwb"
        extra = " | ffmpeg %s %.3f s" % (lib, best([FF, "-nostdin", "-v", "error", "-threads", "1", "-c:a", lib, "-i", path, "-map", "0:a:0", "-f", "f32le", "-"]))
    print("%-26s ours %.3f s | ffmpeg %.3f s | %.2fx%s" % (label, ours, theirs, theirs / ours, extra))
