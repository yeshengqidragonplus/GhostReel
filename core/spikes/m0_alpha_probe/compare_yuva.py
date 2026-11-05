"""M0 spike helper: compare two raw yuva420p dumps, and/or an alpha plane against ground truth.

Run isolated:  python -I compare_yuva.py ...

  compare_yuva.py exact  A.yuva B.yuva WIDTH HEIGHT
      Per-frame, per-plane (Y,U,V,A) byte-exact comparison; reports mismatches.

  compare_yuva.py truth  A.yuva TRUTH.gray WIDTH HEIGHT
      Compares the A plane of a yuva dump against a raw 8-bit gray ground truth:
      max / mean absolute error, PSNR, and the value mapping at 0 / 255.

Frame layout (both tools): Y (w*h), U (w/2*h/2), V (w/2*h/2), A (w*h), tightly packed.
"""
import math
import sys


def planes(w, h):
    cw, ch = (w + 1) // 2, (h + 1) // 2
    return [("Y", w * h), ("U", cw * ch), ("V", cw * ch), ("A", w * h)]


def cmd_exact(a_path, b_path, w, h):
    layout = planes(w, h)
    fsize = sum(n for _, n in layout)
    a, b = open(a_path, "rb").read(), open(b_path, "rb").read()
    print(f"A: {len(a)} bytes ({len(a) / fsize:.2f} frames)   B: {len(b)} bytes ({len(b) / fsize:.2f} frames)")
    if len(a) != len(b) or len(a) % fsize:
        print("RESULT: FAIL (size mismatch)")
        return 1
    bad = {name: 0 for name, _ in layout}
    first_bad = {}
    for f in range(len(a) // fsize):
        off = f * fsize
        for name, n in layout:
            if a[off:off + n] != b[off:off + n]:
                bad[name] += 1
                first_bad.setdefault(name, f)
            off += n
    for name, _ in layout:
        print(f"plane {name}: {bad[name]} differing frames" + (f" (first: frame {first_bad[name]})" if name in first_bad else ""))
    ok = not any(bad.values())
    print("RESULT:", "PASS (bit-exact, all 4 planes, all frames)" if ok else "FAIL")
    return 0 if ok else 1


def cmd_truth(a_path, t_path, w, h):
    layout = planes(w, h)
    fsize = sum(n for _, n in layout)
    apx = w * h
    a, t = open(a_path, "rb").read(), open(t_path, "rb").read()
    frames = len(a) // fsize
    if len(t) != frames * apx:
        print(f"RESULT: FAIL (truth has {len(t)} bytes, expected {frames * apx})")
        return 1
    tot_abs = tot_sq = 0
    max_err = 0
    worst_frame = 0
    # confusion at the extremes: how are true-0 and true-255 pixels decoded?
    z_min, z_max = 255, 0
    z_sum = z_n = f_sum = f_n = 0
    f_min = 255
    f_max = 0
    for f in range(frames):
        da = a[f * fsize + fsize - apx: (f + 1) * fsize]
        tr = t[f * apx:(f + 1) * apx]
        fmax = 0
        for x, y in zip(da, tr):
            d = x - y
            if d < 0:
                d = -d
            tot_abs += d
            tot_sq += d * d
            if d > fmax:
                fmax = d
            if y == 0:
                z_n += 1
                z_sum += x
                if x > z_max:
                    z_max = x
                if x < z_min:
                    z_min = x
            elif y == 255:
                f_n += 1
                f_sum += x
                if x < f_min:
                    f_min = x
                if x > f_max:
                    f_max = x
        if fmax > max_err:
            max_err, worst_frame = fmax, f
    n = frames * apx
    mse = tot_sq / n
    psnr = float("inf") if mse == 0 else 10 * math.log10(255 * 255 / mse)
    print(f"frames: {frames}, alpha pixels compared: {n}")
    print(f"mean abs error: {tot_abs / n:.4f}   max abs error: {max_err} (frame {worst_frame})   PSNR: {psnr:.2f} dB")
    if z_n:
        print(f"true alpha == 0   ({z_n} px): decoded min={z_min} max={z_max} mean={z_sum / z_n:.3f}")
    if f_n:
        print(f"true alpha == 255 ({f_n} px): decoded min={f_min} max={f_max} mean={f_sum / f_n:.3f}")
    return 0


def main(argv):
    try:
        mode, p1, p2, w, h = argv[1], argv[2], argv[3], int(argv[4]), int(argv[5])
    except (IndexError, ValueError):
        print(__doc__)
        return 2
    if mode == "exact":
        return cmd_exact(p1, p2, w, h)
    if mode == "truth":
        return cmd_truth(p1, p2, w, h)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
