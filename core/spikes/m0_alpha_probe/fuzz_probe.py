"""M0 spike helper: crash-hunting for alpha_probe. Deterministic (fixed seeds).

Run isolated:  python -I fuzz_probe.py PROBE.exe INPUT.webm WORKDIR [N_FILE_CASES] [N_ALPHA_SEEDS]

Two campaigns:
  A. file-level  : truncations, random byte flips, zeroed/garbage regions of the .webm
  B. alpha-level : alpha_probe --corrupt-alpha SEED (colour intact, alpha payloads corrupted)

A run is a *crash* if the process dies from a signal / NT exception (e.g. 0xC0000005),
and a *hang* if it exceeds the timeout. Exit codes 0,1,2,3 are clean outcomes
(see alpha_probe.cpp). Crashing inputs are kept in WORKDIR/fail/.
"""
import os
import random
import subprocess
import sys
from collections import Counter

CLEAN = {0: "ok", 1: "clean-fail", 2: "usage/io", 3: "caught-exception"}
TIMEOUT_S = 30


def classify(rc):
    if rc in CLEAN:
        return CLEAN[rc]
    return f"CRASH(rc={rc & 0xFFFFFFFF:#x})"


def run(probe, path, extra=()):
    try:
        p = subprocess.run([probe, path, *extra], capture_output=True, timeout=TIMEOUT_S)
        return classify(p.returncode)
    except subprocess.TimeoutExpired:
        return "HANG"


def mutate(data, rng, kind):
    b = bytearray(data)
    n = len(b)
    if kind == "truncate":
        return bytes(b[: rng.choice([0, 1, 4, 16, 64, 200, 1000, rng.randrange(0, n)])])
    if kind == "flip":
        for _ in range(rng.choice([1, 2, 4, 8, 16])):
            # bias towards the first 8 KB (EBML header, tracks, first cluster) half the time
            pos = rng.randrange(0, 8192) if rng.random() < 0.5 else rng.randrange(0, n)
            b[min(pos, n - 1)] ^= rng.randrange(1, 256)
        return bytes(b)
    if kind == "region":
        start = rng.randrange(0, n - 1)
        ln = rng.choice([16, 256, 4096])
        fill = rng.choice([0x00, 0xFF, None])
        for i in range(start, min(n, start + ln)):
            b[i] = rng.randrange(256) if fill is None else fill
        return bytes(b)
    raise ValueError(kind)


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 2
    probe, src, work = argv[1], argv[2], argv[3]
    n_file = int(argv[4]) if len(argv) > 4 else 150
    n_alpha = int(argv[5]) if len(argv) > 5 else 100
    os.makedirs(os.path.join(work, "fail"), exist_ok=True)
    data = open(src, "rb").read()
    rng = random.Random(20261105)

    print(f"A. file-level: {n_file} mutated copies of {os.path.basename(src)} ({len(data)} bytes)")
    tally = Counter()
    per_kind = {}
    for i in range(n_file):
        kind = ("truncate", "flip", "region")[i % 3]
        m = mutate(data, rng, kind)
        path = os.path.join(work, "case.webm")
        open(path, "wb").write(m)
        res = run(probe, path)
        tally[res] += 1
        per_kind.setdefault(kind, Counter())[res] += 1
        if res.startswith("CRASH") or res == "HANG":
            keep = os.path.join(work, "fail", f"file_{i}_{kind}.webm")
            open(keep, "wb").write(m)
            print(f"   !! {res} on case {i} ({kind}) -> {keep}")
    for k, c in per_kind.items():
        print(f"   {k:9s}: " + ", ".join(f"{r}={v}" for r, v in sorted(c.items())))
    print("   TOTAL    : " + ", ".join(f"{r}={v}" for r, v in sorted(tally.items())))

    print(f"B. alpha-level: {n_alpha} seeds of --corrupt-alpha (colour intact)")
    tally_b = Counter()
    for seed in range(1, n_alpha + 1):
        res = run(probe, src, ("--corrupt-alpha", str(seed)))
        tally_b[res] += 1
        if res.startswith("CRASH") or res == "HANG":
            print(f"   !! {res} with seed {seed}")
    print("   TOTAL    : " + ", ".join(f"{r}={v}" for r, v in sorted(tally_b.items())))

    bad = sum(v for r, v in (tally + tally_b).items() if r.startswith("CRASH") or r == "HANG")
    print("RESULT:", "PASS (no crash, no hang)" if bad == 0 else f"FAIL ({bad} crashes/hangs)")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
