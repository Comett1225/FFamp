import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
AMEDIA = os.environ.get("AMEDIA", os.path.join(HERE, "..", "build", "amedia"))


def variants(data, count, rng):
    size = len(data)
    for n in range(count):
        kind = n % 6
        if kind == 0:
            yield data[:rng.randrange(size)]
        elif kind == 1:
            out = bytearray(data)
            for _ in range(rng.randrange(1, 9)):
                out[rng.randrange(size)] ^= 1 << rng.randrange(8)
            yield bytes(out)
        elif kind == 2:
            out = bytearray(data)
            at = rng.randrange(size)
            out[at:at + 4] = b"\xff" * len(out[at:at + 4])
            yield bytes(out)
        elif kind == 3:
            at = rng.randrange(size)
            length = rng.randrange(1, 2000)
            yield data[:at] + data[at + length:]
        elif kind == 4:
            at = rng.randrange(min(size, 600))
            out = bytearray(data)
            out[at] = rng.randrange(256)
            yield bytes(out)
        else:
            at = rng.randrange(size)
            yield data[:at] + bytes(rng.randrange(256) for _ in range(rng.randrange(1, 64))) + data[at:]


def main():
    target = sys.argv[1]
    count = int(sys.argv[2])
    rng = random.Random(int(os.environ.get("SEED", "1234")))
    runs = opened = 0
    bad = []
    environment = dict(os.environ)
    if os.environ.get("GUARD"):
        environment["DYLD_INSERT_LIBRARIES"] = "/usr/lib/libgmalloc.dylib"
    for path in sys.argv[3:]:
        data = open(path, "rb").read()
        for n, changed in enumerate(variants(data, count, rng)):
            with open(target, "wb") as out:
                out.write(changed)
            try:
                result = subprocess.run([AMEDIA, "packets", target], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=30, env=environment)
                code = result.returncode
                said = result.stderr.strip()
            except subprocess.TimeoutExpired:
                code = -1000
                said = b"timeout"
            runs += 1
            opened += code == 0
            if code not in (0, 1) or (code == 1 and b"amedia: " not in said):
                bad.append(f"{os.path.basename(path)} variant {n} exit {code}")
                if os.environ.get("KEEP"):
                    with open(f"{target}.{os.path.basename(path)}.{n}", "wb") as out:
                        out.write(changed)
    os.remove(target)
    print(f"broken files: {runs} tried, {opened} still opened, {runs - opened - len(bad)} refused with a message, {len(bad)} crashed or hung" + "".join("; " + b for b in bad[:10]))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
