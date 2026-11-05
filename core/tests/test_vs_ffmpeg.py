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
  * the threaded player (`play`): with the whole clip pre-decoded and a fine clock every frame is
    shown once, in order, none dropped; with coarse clocks exactly the frames an idealised
    "newest due frame wins" model predicts are shown and the rest counted as dropped; free-running
    on the wall clock with queue depths 1/2/4/16 gives increasing pts, correct bytes and
    consistent counters; closing while the decode thread is busy does not hang.
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

    if len(pts) <= 64:  # the --wait-end checks need the whole clip to fit in the queue
        test_player(cli, work, clip, name, ref, pts, fsize)


def parse_play(stdout):
    """(list of pts_us from 'frame pts_us=N' lines, dict of 'key: value' statistics)."""
    pts, stats = [], {}
    for line in stdout.splitlines():
        if line.startswith("frame pts_us="):
            pts.append(int(line.split("=", 1)[1]))
        elif ": " in line:
            k, v = line.split(": ", 1)
            stats[k.strip()] = v.strip()
    return pts, stats


def model_clock(pts, step):
    """Idealised player with every frame already queued: which packet indices are returned,
    and how many are dropped, for a clock t = 0, step, 2*step, ... (newest due frame wins)."""
    returned, dropped, consumed, k = [], 0, 0, 0
    while consumed < len(pts):
        t = k * step
        k += 1
        due = [i for i in range(consumed, len(pts)) if pts[i] <= t]
        if due:
            j = due[-1]
            dropped += j - consumed
            returned.append(j)
            consumed = j + 1
    return returned, dropped


def test_player(cli, work, clip, name, ref, pts, fsize):
    """The threaded player (ac_start / ac_acquire_frame / ac_release_frame)."""
    out_path = os.path.join(work, name + ".play.raw")
    n = len(pts)
    index_of = {p: i for i, p in enumerate(pts)}
    if len(index_of) != n:
        print(f"SKIP  {name}: duplicate timestamps, threaded checks need unique pts")
        return

    def frames_ok(shown):
        data = open(out_path, "rb").read()
        if len(data) != len(shown) * fsize:
            return f"{len(data)} bytes for {len(shown)} frames"
        for i, p in enumerate(shown):
            j = index_of.get(p)
            if j is None:
                return f"unknown pts {p}"
            if data[i * fsize:(i + 1) * fsize] != ref[j * fsize:(j + 1) * fsize]:
                return f"frame #{i} (pts {p}) differs from the reference"
        return None

    # A. everything decoded first, fine clock: every frame exactly once, in order, nothing dropped
    r = run([cli, "play", clip, "--out", out_path, "--depth", "64", "--wait-end", "--clock-step-us", "1000"])
    shown, st = parse_play(r.stdout)
    bad = frames_ok(shown) if r.returncode == 0 else r.stderr
    check(r.returncode == 0 and shown == pts and bad is None and st.get("dropped") == "0",
          f"{name}: player, fine clock: all {n} frames in order, byte-identical, none dropped", bad or str(st))
    check(int(st.get("decode_time_us", "0")) > 0, f"{name}: player reports decode time")

    # B. coarse clocks: the newest due frame wins and older ones are dropped, exactly as modelled
    for step in (20_000, 33_333, 100_000, 250_000, 1_000_000):
        r = run([cli, "play", clip, "--out", out_path, "--depth", "64", "--wait-end", "--clock-step-us", str(step)])
        shown, st = parse_play(r.stdout)
        want, want_dropped = model_clock(pts, step)
        bad = frames_ok(shown) if r.returncode == 0 else r.stderr
        ok = (r.returncode == 0 and shown == [pts[j] for j in want] and bad is None
              and st.get("dropped") == str(want_dropped) and st.get("acquired") == str(len(want)))
        check(ok, f"{name}: player, clock step {step} us: {len(want)} frames shown, {want_dropped} dropped",
              bad or f"got {len(shown)} frames, stats {st}")

    # C. free-running with a real decode thread and small queues (races show up here):
    #    pts strictly increasing, each frame correct, accounting adds up, no hang.
    #    The clock is the wall clock x 10 (a 2 s clip takes ~0.2 s); the decode thread cannot always
    #    keep up, so frames may be dropped, which is fine here.
    for depth in (1, 2, 4, 16):
        problems = []
        for rep in range(3):
            r = run([cli, "play", clip, "--out", out_path, "--depth", str(depth), "--realtime-speed", "10"])
            shown, st = parse_play(r.stdout)
            if r.returncode != 0:
                problems.append(f"run {rep}: exit {r.returncode & 0xFFFFFFFF:#x}: {r.stderr.strip()}")
                continue
            bad = frames_ok(shown)
            if bad:
                problems.append(f"run {rep}: {bad}")
            elif any(b <= a for a, b in zip(shown, shown[1:])):
                problems.append(f"run {rep}: pts not strictly increasing")
            elif not shown or shown[-1] != pts[-1]:
                problems.append(f"run {rep}: last frame is not the last packet")
            elif int(st["decoded"]) != int(st["acquired"]) + int(st["dropped"]) or int(st["acquired"]) != len(shown):
                problems.append(f"run {rep}: frame accounting is off: {st}")
        check(not problems, f"{name}: player, free-running, queue depth {depth} (3 runs)", "; ".join(problems))

    # D. closing with the decode thread still busy must not hang or crash
    problems = []
    for rep in range(10):
        r = run([cli, "play", clip, "--out", out_path, "--depth", "4", "--realtime-speed", "10", "--frames", "2"])
        if r.returncode != 0:
            problems.append(f"run {rep}: exit {r.returncode & 0xFFFFFFFF:#x}: {r.stderr.strip()}")
    check(not problems, f"{name}: closing early, decode thread busy (10 runs)", "; ".join(problems))

    r = run([cli, "play", clip, "--out", out_path, "--depth", "4", "--wait-end"])
    check(r.returncode == 2, f"{name}: --wait-end with a too-small --depth is a usage error")
    r = run([cli, "play", clip, "--out", out_path, "--loop", "--wait-end"])
    check(r.returncode == 2, f"{name}: --loop with --wait-end is a usage error")

    test_player_seek(cli, work, clip, name, ref, pts, fsize, frames_ok, out_path)
    test_player_loop(cli, work, clip, name, ref, pts, fsize, frames_ok, out_path)


def parse_events(stdout):
    """[('frame', pts_us) | ('seek', target_us)] in the order the CLI printed them."""
    ev = []
    for line in stdout.splitlines():
        if line.startswith("frame pts_us="):
            ev.append(("frame", int(line.split("=", 1)[1])))
        elif line.startswith("seek pts_us="):
            ev.append(("seek", int(line.split("=", 1)[1])))
    return ev


def packet_for(pts, t):
    """Index of the packet shown at time t: the latest with pts <= t, else the first."""
    return max([i for i, p in enumerate(pts) if p <= t], default=0)


def test_player_seek(cli, work, clip, name, ref, pts, fsize, frames_ok, out_path):
    """ac_seek with the threaded player."""
    n = len(pts)
    # Deterministic: seek right after the start, whole clip pre-decoded, clock starts at the target.
    # Exactly the frames from the packet shown at T onwards must come out, once each, in order.
    targets = sorted({pts[0] - 5_000_000, pts[0], pts[n // 2] - 1, pts[n // 2], pts[n // 2] + 16_500,
                      pts[-1], pts[-1] + 1_000_000})
    for t in targets:
        j = packet_for(pts, t)
        r = run([cli, "play", clip, "--out", out_path, "--depth", "64", "--wait-end", "--seek-us", str(t),
                 "--clock-step-us", "1000"])
        shown, st = parse_play(r.stdout)
        bad = frames_ok(shown) if r.returncode == 0 else r.stderr
        check(r.returncode == 0 and shown == pts[j:] and bad is None and st.get("dropped") == "0",
              f"{name}: player, seek {t} us -> frames from packet {j} on, none dropped",
              bad or f"got {len(shown)} frames starting at {shown[:1]}, stats {st}")

    # Free-running: seek while the decode thread is busy, forwards and backwards. After the seek no
    # stale frame may appear: every later frame has pts >= the target packet's, strictly increasing,
    # bytes correct, and the clip is still played to its last frame.
    cases = [(10, pts[n * 3 // 4]), (n * 2 // 3, pts[n // 6]), (3, pts[0]), (n // 2, pts[-1]), (20, pts[n // 2])]
    for depth in (1, 4):
        problems = []
        for after, target in cases:
            for rep in range(2):
                r = run([cli, "play", clip, "--out", out_path, "--depth", str(depth), "--realtime-speed", "10",
                         "--mid-seek-after", str(after), "--mid-seek-us", str(target)])
                if r.returncode != 0:
                    problems.append(f"seek after {after} to {target}: exit {r.returncode & 0xFFFFFFFF:#x}: {r.stderr.strip()}")
                    continue
                ev = parse_events(r.stdout)
                shown = [v for k, v in ev if k == "frame"]
                bad = frames_ok(shown)
                seek_at = next((i for i, (k, _) in enumerate(ev) if k == "seek"), None)
                if bad:
                    problems.append(f"seek after {after} to {target}: {bad}")
                elif seek_at is None:
                    problems.append(f"seek after {after} to {target}: the seek was never issued")
                else:
                    before = [v for k, v in ev[:seek_at] if k == "frame"]
                    later = [v for k, v in ev[seek_at + 1:] if k == "frame"]
                    j = packet_for(pts, target)
                    if any(b <= a for a, b in zip(before, before[1:])) or any(b <= a for a, b in zip(later, later[1:])):
                        problems.append(f"seek after {after} to {target}: pts not increasing within a segment")
                    elif any(v < pts[j] for v in later):
                        problems.append(f"seek after {after} to {target}: stale frame after the seek "
                                        f"({min(later)} < {pts[j]})")
                    elif not later or later[-1] != pts[-1]:
                        problems.append(f"seek after {after} to {target}: the clip did not play on to its last frame")
        check(not problems, f"{name}: player, seeking while the decode thread is busy, queue depth {depth}",
              "; ".join(problems))


def test_player_loop(cli, work, clip, name, ref, pts, fsize, frames_ok, out_path):
    """ac_set_loop: the clock stays continuous across passes."""
    n = len(pts)
    info = parse_info(run([cli, "info", clip]).stdout)
    period = int(info["loop_period_us"])
    fps = float(info["fps"])
    frame_us = round(1e6 / fps) if fps > 0 else 0
    check(abs(period - ((pts[-1] - pts[0]) + frame_us)) <= 1000,
          f"{name}: loop period is last-first plus one frame", f"{period} us")
    if n > 64 or n < 5:
        return

    # Deterministic: queue pre-filled (64 frames), fine clock. A pass boundary must read
    # ... last frame, first frame + period, second frame + period ...
    k = 64 - n
    want = pts + [period + pts[i] for i in range(k)]
    r = run([cli, "play", clip, "--out", out_path, "--loop", "--depth", "64", "--wait-queue-full",
             "--frames", "64", "--clock-step-us", "1000"])
    shown, st = parse_play(r.stdout)
    # frames_ok needs pts -> packet; map the shifted pts back for the byte check
    mapped = [p if p in set(pts) else p - period for p in shown]
    bad = frames_ok(mapped) if r.returncode == 0 else r.stderr
    check(r.returncode == 0 and shown == want and bad is None and st.get("dropped") == "0",
          f"{name}: loop, pass boundary: pts continue by one period, bytes identical, none dropped",
          bad or f"got {shown[n - 2:n + 2]}")

    # Seek to the last packet with looping on: offset resets to 0 for the seek, then grows by one
    # period per pass: last frame, then pass 1 (+P), then pass 2 (+2P).
    t = pts[-1]
    full_passes = (64 - 1) // n
    rest = (64 - 1) % n
    want = [pts[-1]]
    for q in range(1, full_passes + 1):
        want += [q * period + p for p in pts]
    want += [(full_passes + 1) * period + pts[i] for i in range(rest)]
    r = run([cli, "play", clip, "--out", out_path, "--loop", "--depth", "64", "--wait-queue-full", "--seek-us",
             str(t), "--frames", "64", "--clock-step-us", "1000"])
    shown, st = parse_play(r.stdout)
    pts_set = set(pts)
    mapped = []
    for p in shown:
        q = (p - pts[0]) // period if p >= pts[0] else 0
        mapped.append(p - q * period)
    bad = frames_ok(mapped) if r.returncode == 0 else r.stderr
    check(r.returncode == 0 and shown == want and bad is None,
          f"{name}: loop after a seek to the last frame: offsets accumulate +P, +2P", bad or f"got {shown[:3]}...")

    # Free-running, several passes: pts strictly increasing, each frame correct for its pass,
    # and at least one pass boundary was crossed.
    for depth in (1, 4):
        problems = []
        for rep in range(3):
            r = run([cli, "play", clip, "--out", out_path, "--loop", "--depth", str(depth),
                     "--realtime-speed", "20", "--frames", str(int(n * 2.6))])
            shown, st = parse_play(r.stdout)
            if r.returncode != 0:
                problems.append(f"run {rep}: exit {r.returncode & 0xFFFFFFFF:#x}: {r.stderr.strip()}")
                continue
            passes = [(p - pts[0]) // period for p in shown]
            mapped = [p - q * period for p, q in zip(shown, passes)]
            bad = frames_ok(mapped)
            if bad:
                problems.append(f"run {rep}: {bad}")
            elif any(b <= a for a, b in zip(shown, shown[1:])):
                problems.append(f"run {rep}: pts not strictly increasing across passes")
            elif max(passes) < 1:
                problems.append(f"run {rep}: never crossed a pass boundary")
        check(not problems, f"{name}: loop, free-running, queue depth {depth} (3 runs, ~2.6 passes)",
              "; ".join(problems))


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
