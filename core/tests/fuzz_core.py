"""M1 core robustness: deterministic mutation fuzzing of the alphaclip CLI.

Run isolated:
  python -I core/tests/fuzz_core.py --cli build/core-win64/bin/alphaclip.exe --workdir <empty dir> \
      clip.webm [--cases N] [--seed S]

Each case mutates the clip (fixed seed, so a failure reproduces) and runs `decode` or the threaded
`play` on it. Clean outcomes are exit code 0 (still decodable), 1 (error reported) or 2 (usage);
anything else is a crash (access violation, abort, ...), and a run that exceeds the time limit is
a hang. Failing inputs are kept in <workdir>/fail/ together with the command that triggered them.

Mutations: truncation; random byte flips (half of them inside the first 4 KB: EBML header, Segment,
Info, Tracks); zeroed / 0xFF / random regions; and "structure hits", i.e. byte flips placed right
after each EBML element ID of interest (Cluster, SimpleBlock, BlockGroup, BlockAdditions, Cues, ...)
where the size field lives, so that the size parsing and the bound checks get exercised.
"""
import argparse
import os
import random
import subprocess
import sys
from collections import Counter

CLEAN = {0: "ok", 1: "error", 2: "usage"}
TIMEOUT_S = 20
# EBML element IDs whose following bytes are a size field.
IDS = {
    "Segment": bytes.fromhex("18538067"), "Info": bytes.fromhex("1549a966"),
    "Tracks": bytes.fromhex("1654ae6b"), "TrackEntry": bytes.fromhex("ae"),
    "Video": bytes.fromhex("e0"), "Cluster": bytes.fromhex("1f43b675"),
    "SimpleBlock": bytes.fromhex("a3"), "BlockGroup": bytes.fromhex("a0"),
    "Block": bytes.fromhex("a1"), "BlockAdditions": bytes.fromhex("75a1"),
    "BlockMore": bytes.fromhex("a6"), "BlockAdditional": bytes.fromhex("a5"),
    "Cues": bytes.fromhex("1c53bb6b"), "PixelWidth": bytes.fromhex("b0"),
    "PixelHeight": bytes.fromhex("ba"), "AlphaMode": bytes.fromhex("53c0"),
}


def classify(rc):
    return CLEAN.get(rc, f"CRASH(rc={rc & 0xFFFFFFFF:#x})")


def run(cmd):
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=TIMEOUT_S)
        return classify(p.returncode)
    except subprocess.TimeoutExpired:
        return "HANG"


def find_all(data, needle, limit=400):
    out, i = [], data.find(needle)
    while i != -1 and len(out) < limit:
        out.append(i)
        i = data.find(needle, i + 1)
    return out


def mutate(data, rng, kind):
    b = bytearray(data)
    n = len(b)
    if kind == "truncate":
        return bytes(b[: rng.choice([0, 1, 4, 16, 64, 200, 1000, rng.randrange(0, n)])])
    if kind == "flip":
        for _ in range(rng.choice([1, 2, 4, 8, 16])):
            pos = rng.randrange(0, 4096) if rng.random() < 0.5 else rng.randrange(0, n)
            b[min(pos, n - 1)] ^= rng.randrange(1, 256)
        return bytes(b)
    if kind == "region":
        start = rng.randrange(0, n - 1)
        for i in range(start, min(n, start + rng.choice([8, 64, 512, 4096]))):
            fill = rng.choice([0x00, 0xFF, None])
            b[i] = rng.randrange(256) if fill is None else fill
        return bytes(b)
    if kind == "structure":
        name = rng.choice(list(IDS))
        hits = find_all(data, IDS[name])
        if not hits:
            return mutate(data, rng, "flip")
        pos = rng.choice(hits) + len(IDS[name])  # first byte(s) of the size field
        for k in range(rng.choice([1, 1, 2, 4])):
            if pos + k < n:
                b[pos + k] = rng.choice([0x00, 0x01, 0x7F, 0x80, 0xFF, rng.randrange(256)])
        return bytes(b)
    raise ValueError(kind)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--cases", type=int, default=1000)
    ap.add_argument("--seed", type=int, default=20261105)
    ap.add_argument("clip")
    a = ap.parse_args()
    cli, work, clip = (os.path.abspath(x) for x in (a.cli, a.workdir, a.clip))
    os.makedirs(os.path.join(work, "fail"), exist_ok=True)
    data = open(clip, "rb").read()
    rng = random.Random(a.seed)
    kinds = ("truncate", "flip", "region", "structure", "structure")
    out = os.path.join(work, "fuzz.raw")
    case_path = os.path.join(work, "case.webm")

    tally = Counter()
    per_kind = {}
    bad = 0
    for i in range(a.cases):
        kind = kinds[i % len(kinds)]
        mutated = mutate(data, rng, kind)
        open(case_path, "wb").write(mutated)
        # alternate the synchronous decode and the threaded player
        if i % 4 == 3:
            cmd = [cli, "play", case_path, "--out", out, "--depth", "8", "--realtime-speed", "50", "--timeout-ms", "10000"]
        else:
            cmd = [cli, "decode", case_path, "--out", out]
        res = run(cmd)
        tally[res] += 1
        per_kind.setdefault(kind, Counter())[res] += 1
        if res.startswith("CRASH") or res == "HANG":
            bad += 1
            keep = os.path.join(work, "fail", f"case_{i}_{kind}.webm")
            open(keep, "wb").write(mutated)
            print(f"!! {res} on case {i} ({kind}): {' '.join(cmd[:2])} ... -> {keep}")
    print(f"{a.cases} cases on {os.path.basename(clip)} (seed {a.seed})")
    for k, c in per_kind.items():
        print(f"  {k:10s}: " + ", ".join(f"{r}={v}" for r, v in sorted(c.items())))
    print("  TOTAL     : " + ", ".join(f"{r}={v}" for r, v in sorted(tally.items())))
    print("RESULT:", "PASS (no crash, no hang)" if bad == 0 else f"FAIL ({bad} crashes/hangs)")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
