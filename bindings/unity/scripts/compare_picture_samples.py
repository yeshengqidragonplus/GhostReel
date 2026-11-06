"""Compares the picture samples of the Unity benchmark app on the phone with the same samples taken in the Windows editor.

    python -I bindings/unity/scripts/compare_picture_samples.py <windows report or log> <device results.txt> [--tol 3]

Both files contain lines with a JSON object that has "clip", "t", "pts_ms", "mean" and "samples" (written by
AlphaClipBenchmark.SamplePicture: 'INFO  verify {...}' in the Windows play-mode report, 'ALPHACLIP_BENCH verify {...}' in the
device log). The Windows picture was compared with an independent CPU reference to within 1 level in M2 (Gamma and Linear),
so agreement here (same clip, same frame, every sampled channel within --tol, the mean within 1 level) says the phone shows
the same picture: decode through the Android .so, texture upload, GPU YUV->RGB. A different GPU may differ by a level or two
in the bilinear chroma filter; the largest difference is printed so the tolerance is not hidden.
Exit code: 0 everything matches, 1 a mismatch or a missing sample set.
"""
import argparse
import json
import re
import sys


def read_samples(path):
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.search(r'(\{"clip":.*\})\s*$', line)
            if not m:
                continue
            try:
                d = json.loads(m.group(1))
            except ValueError:
                continue
            if "samples" in d:
                out[(d["clip"], d["t"])] = d
            elif "error" in d:
                out[(d["clip"], "error")] = d
    return out


def channels(hex_list):
    vals = []
    for px in hex_list.split(","):
        vals += [int(px[0:2], 16), int(px[2:4], 16), int(px[4:6], 16)]
    return vals


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("windows")
    ap.add_argument("device")
    ap.add_argument("--tol", type=int, default=3, help="largest allowed difference of a sampled channel (0-255)")
    args = ap.parse_args()
    win, dev = read_samples(args.windows), read_samples(args.device)
    failures = 0
    for key in sorted(k for k in dev if k[1] == "error"):
        print("FAIL  device could not produce samples for", key[0], ":", dev[key]["error"])
        failures += 1
    keys = sorted(k for k in win if k[1] != "error")
    if not keys:
        print("FAIL  no samples in", args.windows)
        return 1
    worst = 0
    for key in keys:
        if key not in dev:
            print("FAIL  %s t=%s: no samples from the device" % key)
            failures += 1
            continue
        w, d = win[key], dev[key]
        if (w["w"], w["h"]) != (d["w"], d["h"]) or w["pts_ms"] != d["pts_ms"]:
            print("FAIL  %s t=%s: different picture (windows %sx%s pts %s ms, device %sx%s pts %s ms)" %
                  (key + (w["w"], w["h"], w["pts_ms"], d["w"], d["h"], d["pts_ms"])))
            failures += 1
            continue
        cw, cd = channels(w["samples"]), channels(d["samples"])
        diff = max(abs(a - b) for a, b in zip(cw, cd))
        mean_diff = max(abs(a - b) for a, b in zip(w["mean"], d["mean"]))
        worst = max(worst, diff)
        ok = diff <= args.tol and mean_diff <= 1.0 and len(cw) == len(cd)
        failures += 0 if ok else 1
        print("%s  %s t=%s pts %s ms: %d channels sampled, max difference %d, mean difference %.2f" %
              ("PASS" if ok else "FAIL", key[0], key[1], w["pts_ms"], len(cw), diff, mean_diff))
    print("%d sample sets compared, largest sampled difference %d (tolerance %d)" % (len(keys), worst, args.tol))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
