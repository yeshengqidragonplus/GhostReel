"""M1 core test: the alphaclip CLI against an independent ffmpeg decode, plus error paths.

Run isolated:
  python -I core/tests/test_vs_ffmpeg.py --cli build/core-win64/bin/alphaclip.exe \
      --ffmpeg-dir E:/Tools/ffmpeg-9.0.2-full_build/bin --workdir <empty dir> clip.webm [clip2.webm ...]

For every clip it checks:
  * `info` agrees with ffprobe (size, packet count, duration within one frame)
  * `decode` output is byte-identical to ffmpeg's raw decode (yuv420p, or yuva420p when the
    clip has alpha), all frames. The reference MUST be decoded with `-c:v libvpx-vp9` placed
    before `-i`: ffmpeg's default native vp9 decoder silently drops alpha.
  * `decode --seek-us T` equals the ffmpeg frames from the packet shown at T onwards, for targets
    at the start, the middle, between frames, on every keyframe and one tick before it, and
    before / after the whole clip.
And, without ffmpeg: missing file, empty file, garbage, truncated file and bad usage all fail
cleanly (exit code 1 or 2, no crash).
"""
import argparse
import os
import random
import subprocess
import sys

failures = 0
checks = 0


def check(ok, label, detail=""):
    global failures, checks
    checks += 1
    if not ok:
        failures += 1
    print(("PASS  " if ok else "FAIL  ") + label + (("  -- " + detail) if (detail and not ok) else ""))


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def ffprobe_packets(ffprobe, clip):
    """[(pts_us, is_keyframe)] in stream order, from ffprobe (independent of our parser)."""
    p = run([ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries",
             "packet=pts_time,flags", "-of", "csv=p=0", clip])
    out = []
    for line in p.stdout.splitlines():
        parts = line.strip().split(",")
        if len(parts) >= 2 and parts[0]:
            out.append((round(float(parts[0]) * 1_000_000), "K" in parts[1]))
    return out


def parse_info(text):
    d = {}
    for line in text.splitlines():
        if ": " in line:
            k, v = line.split(": ", 1)
            d[k.strip()] = v.strip()
    return d


def first_difference(a, b, w, h, alpha):
    """Describe the first differing (frame, plane) between two raw buffers, or None."""
    cw, ch = (w + 1) // 2, (h + 1) // 2
    planes = [("Y", w * h), ("U", cw * ch), ("V", cw * ch)] + ([("A", w * h)] if alpha else [])
    fsize = sum(n for _, n in planes)
    if len(a) != len(b):
        return f"size {len(a)} vs {len(b)} bytes ({len(a) / fsize:.2f} vs {len(b) / fsize:.2f} frames)"
    for f in range(len(a) // fsize):
        off = f * fsize
        for name, n in planes:
            if a[off:off + n] != b[off:off + n]:
                return f"frame {f}, plane {name}"
            off += n
    return None


def test_clip(cli, ffmpeg, ffprobe, work, clip):
    name = os.path.basename(clip)
    info_run = run([cli, "info", clip])
    check(info_run.returncode == 0, f"{name}: info exits 0", info_run.stderr)
    info = parse_info(info_run.stdout)
    w, h = (int(x) for x in info["size"].split("x"))
    alpha = info["alpha"] == "yes"
    cw, ch = (w + 1) // 2, (h + 1) // 2
    fsize = w * h + 2 * cw * ch + (w * h if alpha else 0)

    packets = ffprobe_packets(ffprobe, clip)
    check(int(info["packets"]) == len(packets), f"{name}: packet count matches ffprobe",
          f"{info['packets']} vs {len(packets)}")

    ref_path = os.path.join(work, name + ".ref.raw")
    pix = "yuva420p" if alpha else "yuv420p"
    r = run([ffmpeg, "-v", "error", "-y", "-c:v", "libvpx-vp9", "-i", clip, "-fps_mode",
             "passthrough", "-pix_fmt", pix, "-f", "rawvideo", ref_path])
    check(r.returncode == 0, f"{name}: ffmpeg reference decode ({pix})", r.stderr)
    ref = open(ref_path, "rb").read()
    nframes = len(ref) // fsize
    check(len(ref) % fsize == 0 and nframes == len(packets), f"{name}: reference has one frame per packet",
          f"{nframes} frames for {len(packets)} packets")
    if nframes != len(packets):
        return

    # duration within one frame of what ffprobe reports for the last packet
    frame_us = 1_000_000 / float(info["fps"]) if float(info["fps"]) > 0 else 0
    check(abs(int(info["duration_us"]) - (packets[-1][0] + frame_us)) <= frame_us + 1000,
          f"{name}: duration is plausible", f"{info['duration_us']} us")

    out_path = os.path.join(work, name + ".out.raw")
    d = run([cli, "decode", clip, "--out", out_path])
    check(d.returncode == 0, f"{name}: decode exits 0", d.stderr)
    diff = first_difference(open(out_path, "rb").read(), ref, w, h, alpha)
    check(diff is None, f"{name}: all {nframes} frames byte-identical to ffmpeg", diff or "")

    d5 = run([cli, "decode", clip, "--out", out_path, "--frames", "5"])
    check(d5.returncode == 0 and os.path.getsize(out_path) == 5 * fsize, f"{name}: --frames 5 writes 5 frames")

    # seek targets
    pts = [p for p, _ in packets]
    keys = [i for i, (_, k) in enumerate(packets) if k]
    targets = {pts[0], pts[len(pts) // 2], pts[-1], pts[-1] + 1_000_000, pts[0] - 5_000_000}
    if len(pts) > 2:
        mid = len(pts) // 2
        targets.add((pts[mid] + pts[mid + 1]) // 2 if mid + 1 < len(pts) else pts[mid])
    for k in keys[:6]:
        targets.add(pts[k])
        targets.add(pts[k] - 1)
    for t in sorted(targets):
        j = max([i for i, p in enumerate(pts) if p <= t], default=0)
        s = run([cli, "decode", clip, "--out", out_path, "--seek-us", str(t)])
        label = f"{name}: seek {t} us -> packet {j}"
        if s.returncode != 0:
            check(False, label, s.stderr)
            continue
        info_out = parse_info(s.stdout)
        diff = first_difference(open(out_path, "rb").read(), ref[j * fsize:], w, h, alpha)
        ok = diff is None and int(info_out["first_pts_us"]) == pts[j]
        check(ok, label, diff or f"first_pts_us {info_out.get('first_pts_us')} != {pts[j]}")


def test_errors(cli, work, good_clip):
    data = open(good_clip, "rb").read()
    out = os.path.join(work, "err.raw")

    def expect_clean_failure(label, args, codes=(1, 2)):
        p = run([cli] + args)
        check(p.returncode in codes, label, f"exit code {p.returncode & 0xFFFFFFFF:#x}, stderr: {p.stderr.strip()}")
        return p

    p = expect_clean_failure("missing file fails cleanly", ["info", os.path.join(work, "does-not-exist.webm")], (1,))
    check("cannot open" in p.stderr, "missing file: message names the problem", p.stderr)

    empty = os.path.join(work, "empty.webm")
    open(empty, "wb").close()
    expect_clean_failure("empty file fails cleanly", ["info", empty], (1,))

    rng = random.Random(20261105)
    garbage = os.path.join(work, "garbage.webm")
    open(garbage, "wb").write(bytes(rng.randrange(256) for _ in range(4096)))
    expect_clean_failure("random bytes fail cleanly", ["info", garbage], (1,))

    for frac in (0.01, 0.1, 0.5, 0.9, 0.999):
        t = os.path.join(work, f"trunc_{frac}.webm")
        open(t, "wb").write(data[: max(1, int(len(data) * frac))])
        expect_clean_failure(f"truncated at {frac:.1%} fails cleanly", ["decode", t, "--out", out], (1,))

    expect_clean_failure("unknown option is a usage error", ["decode", good_clip, "--bogus"], (2,))
    expect_clean_failure("decode without --out is a usage error", ["decode", good_clip], (2,))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--ffmpeg-dir", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("clips", nargs="+")
    a = ap.parse_args()
    # Absolute, native-separator paths: Windows CreateProcess does not resolve "dir/exe" reliably.
    cli = os.path.abspath(a.cli)
    work = os.path.abspath(a.workdir)
    clips = [os.path.abspath(c) for c in a.clips]
    os.makedirs(work, exist_ok=True)
    ffmpeg = os.path.abspath(os.path.join(a.ffmpeg_dir, "ffmpeg.exe" if os.name == "nt" else "ffmpeg"))
    ffprobe = os.path.abspath(os.path.join(a.ffmpeg_dir, "ffprobe.exe" if os.name == "nt" else "ffprobe"))
    for clip in clips:
        test_clip(cli, ffmpeg, ffprobe, work, clip)
    test_errors(cli, work, clips[0])
    print(f"\n{checks - failures}/{checks} checks passed")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
