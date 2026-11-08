#!/usr/bin/env python3
"""Fixed test vectors for the print-area routines (src/xid8300_areas.h).

  golden.py AREACHECK GOLDEN

Each vector is a random plane plus a random list of areas; GOLDEN holds the
SHA-256 of the expected result for the colour-plane cut-out (peel0) and the
peel-off plane (peel1).  The expected results were recorded from the
original Windows driver's output for the same inputs, so they pin the
behaviour the printer receives from Windows.
"""

import hashlib
import os
import random
import subprocess
import sys
import tempfile

W, H = 1036, 664
N = W * H


def area_list(seed):
    rnd = random.Random(1000 + seed)
    out = []
    for _ in range(rnd.randint(1, 8)):
        out.append((rnd.choice((1, 1, 1, 0)), rnd.randint(0, W - 20), rnd.randint(0, H - 20),
                    rnd.randint(1, 400), rnd.randint(1, 200), rnd.choice((1, 1, 2, 3)),
                    rnd.choice((1, 2))))
    return out


def cases():
    for seed in range(12):
        for peel in (0, 1):
            yield "areas-%d-peel%d" % (seed, peel), seed, peel


def run(areacheck, seed, peel, tmp):
    src, dst, areas = (os.path.join(tmp, n) for n in ("in", "out", "areas"))
    open(src, "wb").write(random.Random(seed).randbytes(N))
    with open(areas, "w") as f:
        for a in area_list(seed):
            f.write(" ".join(map(str, a)) + "\n")
    subprocess.check_call([areacheck, str(peel), str(1 + seed % 2), "225", "160", "96", areas, src, dst])
    return open(dst, "rb").read()


def main():
    areacheck, golden_path = sys.argv[1], sys.argv[2]
    golden = dict(l.split()[::-1] for l in open(golden_path) if l.strip())
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, seed, peel in cases():
            if hashlib.sha256(run(areacheck, seed, peel, tmp)).hexdigest() != golden[name]:
                bad += 1
                print("FAIL area routine differs from the recorded result: " + name)
    print(("ok   " if not bad else "FAIL ") + "print-area routines match the recorded results (%d vectors)"
          % len(golden))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
