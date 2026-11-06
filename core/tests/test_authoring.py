"""The authoring commands of the alphaclip CLI (`encode`, `inspect`, `verify`). M5.

Run isolated (needs ffmpeg and numpy; nothing is downloaded, the source footage is generated here):
  python -I core/tests/test_authoring.py --cli build/core-win64/bin/alphaclip.exe \
      --ffmpeg-dir E:/Tools/ffmpeg-9.0.2-full_build/bin --workdir <empty dir> [--truth samples/m4/truth_rgba_3f.raw]

What is tested
  A  footage: an RGBA PNG sequence (two soft-edged discs, saturated colour that is defined everywhere), the same as a
     ProRes 4444 MOV, an odd-sized sequence, an opaque sequence; the M4 clip with a VP9 alpha stream as an input.
  B  `encode` in every mode and colour handling: exit code, files, sidecar content, `inspect` of the result, the
     tool's own verify, and the picture itself: the decoded clip is turned back into RGBA on the CPU (matrix, range,
     premultiplication, packed alpha) and composited over three backgrounds against the exact source; negative controls
     (the wrong premultiplied flag, the wrong matrix) must be caught by the same measurement.
  C  options: --gop, --fps, --matrix, --range, --loop, UTF-8 paths, --force, and every refusal (no alpha to store, odd
     size for packed, missing --fps, bad values, missing ffmpeg, output that exists).
  D  `verify` / `inspect` on files made by hand (a small WebM muxer below): aligned streams pass; colour and alpha
     keyframes that do not line up (risk R10), missing Cues, a cue at the wrong place or on a non-keyframe (R9), equal
     timestamps, a corrupt packet, a constant alpha, and a premultiplied flag that contradicts the picture.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys

import numpy as np

failures = checks = 0


def check(ok, label, detail=""):
    global failures, checks
    checks += 1
    if not ok:
        failures += 1
    print(("PASS  " if ok else "FAIL  ") + label + (("  -- " + str(detail)) if (detail and not ok) else ""), flush=True)


CLI = FFDIR = FFMPEG = WORK = None
W, H, FPS, NFRAMES = 320, 180, 30, 60
TRUTH_FRAMES = (0, 20, 40)

# The source shape: the M4 test shape (samples/m4/gen_clips.ps1), so the exact RGBA truth is the same file.
D1 = "clip((62-hypot(X-160-60*sin(2*T),Y-90-18*cos(2*T)))*255/14,0,255)"
D2 = "clip((22-hypot(X-70-30*cos(3*T),Y-135))*255/10,0,255)"
ALPHA = f"max({D1},{D2})"
RED = "210+45*sin(X/17+3*T)"
GREEN = "70+60*sin(Y/23+T)"
BLUE = "40+40*sin((X+Y)/29)"


def run(args, **kw):
    r = subprocess.run([str(a) for a in args], capture_output=True, **kw)
    return r.returncode, r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace")


def ffmpeg(*args):
    rc, out, err = run([FFMPEG, "-hide_banner", "-v", "error", "-y", *args])
    if rc != 0:
        raise RuntimeError("ffmpeg failed: " + " ".join(map(str, args)) + "\n" + err)


def cli(*args):
    return run([CLI, *args])


def kv(text):
    """'key: value' lines -> dict (the first colon splits)."""
    d = {}
    for line in text.splitlines():
        if ": " in line and not line.startswith("["):
            k, v = line.split(": ", 1)
            d[k.strip()] = v.strip()
    return d


def verify_lines(text):
    """The verify report as {check name: (tag, text)}; the last line gives the result."""
    d = {}
    for line in text.splitlines():
        if line.startswith("[") and "] " in line:
            tag = line[1:line.index("]")].strip()
            name, _, rest = line[line.index("] ") + 2:].partition(": ")
            d.setdefault(name, []).append((tag, rest))
    return d


def tag_of(report, name):
    return report.get(name, [("missing", "")])[0][0]


def path(*parts):
    return os.path.join(WORK, *parts)


# ---- A: footage ----------------------------------------------------------------------------------------------------

def src_lavfi(w, h, frames, rate=FPS):
    base = f"nullsrc=s={w}x{h}:r={rate}:d={frames / rate}"
    color = f"{base},format=gbrp,geq=r='{RED}':g='{GREEN}':b='{BLUE}'"
    alpha = f"{base},format=gray,geq=lum='{ALPHA}'"
    return color, alpha


def make_sources():
    os.makedirs(path("src"), exist_ok=True)
    color, alpha = src_lavfi(W, H, NFRAMES)
    ffmpeg("-lavfi", f"{color}[c];{alpha}[a];[c][a]alphamerge,format=rgba", "-start_number", "0", "-frames:v", str(NFRAMES),
           path("src", "f_%04d.png"))
    os.makedirs(path("src_rgb"), exist_ok=True)
    ffmpeg("-lavfi", f"{color},format=rgb24", "-start_number", "0", "-frames:v", "12", path("src_rgb", "f_%04d.png"))
    os.makedirs(path("src_odd"), exist_ok=True)
    color, alpha = src_lavfi(321, 181, 6)
    ffmpeg("-lavfi", f"{color}[c];{alpha}[a];[c][a]alphamerge,format=rgba", "-start_number", "0", "-frames:v", "6",
           path("src_odd", "f_%04d.png"))
    # The same picture as a renderer often writes it: straight alpha, but black RGB where the pixel is fully transparent
    # (the colour does not continue under the transparent pixels). The composite is the same as the truth.
    os.makedirs(path("src_black"), exist_ok=True)
    color, alpha = src_lavfi(W, H, NFRAMES)
    base = f"nullsrc=s={W}x{H}:r={FPS}:d={NFRAMES / FPS}"
    black = (f"{base},format=gbrp,geq=r='({RED})*gt({ALPHA},0)':g='({GREEN})*gt({ALPHA},0)':b='({BLUE})*gt({ALPHA},0)'")
    ffmpeg("-lavfi", f"{black}[c];{alpha}[a];[c][a]alphamerge,format=rgba", "-start_number", "0", "-frames:v", str(NFRAMES),
           path("src_black", "f_%04d.png"))
    # The same footage as a ProRes 4444 movie with alpha (what an artist would export from a compositing tool).
    ffmpeg("-framerate", str(FPS), "-i", path("src", "f_%04d.png"), "-c:v", "prores_ks", "-profile:v", "4444",
           "-pix_fmt", "yuva444p10le", path("src.mov"))
    # The same footage as a WebM with a VP9 alpha stream (what other tools export).
    ffmpeg("-framerate", str(FPS), "-i", path("src", "f_%04d.png"), "-c:v", "libvpx-vp9", "-b:v", "0", "-crf", "4",
           "-g", "30", "-auto-alt-ref", "0", "-lag-in-frames", "0", "-pix_fmt", "yuva420p",
           "-metadata:s:v:0", "alpha_mode=1", path("in_alpha.webm"))
    # Exact RGBA truth of the frames the picture checks look at.
    sel = "+".join(f"eq(n,{n})" for n in TRUTH_FRAMES)
    ffmpeg("-framerate", str(FPS), "-i", path("src", "f_%04d.png"), "-vf", f"select='{sel}'", "-fps_mode", "passthrough",
           "-f", "rawvideo", "-pix_fmt", "rgba", path("truth.raw"))
    raw = np.fromfile(path("truth.raw"), dtype=np.uint8)
    return raw.reshape(len(TRUTH_FRAMES), H, W, 4)


# ---- CPU reference: decoded YUV(A) -> composite over a background ------------------------------------------------

def upsample(plane, h, w):
    """Bilinear, chroma sited between the luma samples (what a GPU does with the R8 plane)."""
    ch, cw = plane.shape
    p = plane.astype(np.float32)

    def idx(n, c):
        pos = (np.arange(n) + 0.5) / 2 - 0.5
        i0 = np.floor(pos).astype(int)
        fr = (pos - i0).astype(np.float32)
        return np.clip(i0, 0, c - 1), np.clip(i0 + 1, 0, c - 1), fr

    y0, y1, fy = idx(h, ch)
    x0, x1, fx = idx(w, cw)
    top = p[y0][:, x0] * (1 - fx) + p[y0][:, x1] * fx
    bot = p[y1][:, x0] * (1 - fx) + p[y1][:, x1] * fx
    return top * (1 - fy)[:, None] + bot * fy[:, None]


def yuv_to_rgb(y, u, v, matrix, full):
    """y: HxW; u, v: already upsampled to HxW. Returns HxWx3 float, 0..255, unclamped."""
    y = y.astype(np.float32)
    if full:
        yy, uu, vv = y, u - 128.0, v - 128.0
    else:
        yy, uu, vv = (y - 16.0) * 255.0 / 219.0, (u - 128.0) * 255.0 / 224.0, (v - 128.0) * 255.0 / 224.0
    if matrix == "bt709":
        r, g, b = yy + 1.5748 * vv, yy - 0.187324 * uu - 0.468124 * vv, yy + 1.8556 * uu
    else:
        r, g, b = yy + 1.402 * vv, yy - 0.344136 * uu - 0.714136 * vv, yy + 1.772 * uu
    return np.stack([r, g, b], axis=-1)


class Decoded:
    """Frames of a clip as the core outputs them, split into planes."""

    def __init__(self, clip, meta_path, info):
        out = path("decoded.raw")
        rc, so, se = cli("decode", clip, "--meta", meta_path, "--out", out)
        if rc != 0:
            raise RuntimeError("decode failed: " + se)
        vw, vh = (int(x) for x in info["video_size"].split("x"))
        self.vw, self.vh = vw, vh
        self.alpha_plane = info["alpha_mode"] == "native_alpha"
        cw, ch = (vw + 1) // 2, (vh + 1) // 2
        fb = vw * vh + 2 * cw * ch + (vw * vh if self.alpha_plane else 0)
        raw = np.fromfile(out, dtype=np.uint8)
        n = raw.size // fb
        assert raw.size == n * fb, "decoded size is not a whole number of frames"
        self.n = n
        self.cw, self.ch = cw, ch
        self.fb = fb
        self.raw = raw

    def frame(self, i):
        f = self.raw[i * self.fb:(i + 1) * self.fb]
        vw, vh, cw, ch = self.vw, self.vh, self.cw, self.ch
        y = f[:vw * vh].reshape(vh, vw)
        u = f[vw * vh:vw * vh + cw * ch].reshape(ch, cw)
        v = f[vw * vh + cw * ch:vw * vh + 2 * cw * ch].reshape(ch, cw)
        a = f[vw * vh + 2 * cw * ch:].reshape(vh, vw) if self.alpha_plane else None
        return y, u, v, a


def rgba_from_decoded(dec, frame_index, meta, premul_override=None, matrix_override=None):
    """-> (rgb HxWx3 float 0..255 as stored (straight or premultiplied), alpha HxW float 0..1, premultiplied flag)."""
    y, u, v, a = dec.frame(frame_index)
    mode = meta["alpha_mode"]
    cw_, ch_ = meta["content_size"]
    full = meta["color_range"] == "full"
    matrix = matrix_override or meta["matrix"]
    yy = y[:ch_, :cw_]
    rgb = yuv_to_rgb(yy, upsample(u, dec.vh, dec.vw)[:ch_, :cw_], upsample(v, dec.vh, dec.vw)[:ch_, :cw_], matrix, full)
    if mode == "native_alpha":
        alpha = a[:ch_, :cw_].astype(np.float32) / 255.0
    elif mode == "packed_right":
        ay = y[:ch_, cw_:2 * cw_].astype(np.float32)
        alpha = np.clip(ay / 255.0 if full else (ay - 16.0) / 219.0, 0, 1)
    elif mode == "packed_bottom":
        ay = y[ch_:2 * ch_, :cw_].astype(np.float32)
        alpha = np.clip(ay / 255.0 if full else (ay - 16.0) / 219.0, 0, 1)
    else:
        alpha = np.ones((ch_, cw_), np.float32)
    premul = meta.get("premultiplied", False) if premul_override is None else premul_override
    return np.clip(rgb, 0, 255), alpha, premul


BACKGROUNDS = ((255, 255, 255), (128, 128, 128), (0, 200, 220))


def composite_stats(rgb, alpha, premul, truth):
    """Mean absolute error and the signed mean error on the soft edge, over three backgrounds (255 = one level)."""
    t_rgb = truth[..., :3].astype(np.float32)
    t_a = truth[..., 3].astype(np.float32) / 255.0
    edge = (t_a > 0.05) & (t_a < 0.95)
    abs_err, edge_err = [], []
    for bg in BACKGROUNDS:
        b = np.array(bg, np.float32)
        out = (rgb if premul else rgb * alpha[..., None]) + b * (1 - alpha[..., None])
        ref = t_rgb * t_a[..., None] + b * (1 - t_a[..., None])
        d = out - ref
        abs_err.append(np.abs(d).mean())
        edge_err.append(d[edge].mean())
    return float(np.mean(abs_err)), float(np.mean(edge_err))


# ---- B: encode in every mode ---------------------------------------------------------------------------------------

MAX_MEAN_ABS = 1.0      # measured 0.12-0.26 at crf 10 (codec noise plus 4:2:0 chroma) on the M4 shape
MAX_EDGE_MEAN = 1.0     # signed: a wrong premultiplication shows up here (measured <= 0.22)
MIN_CONTROL_EDGE = 3.0  # a wrong flag must move the edge mean at least this far (measured 19 / 53)
MIN_CONTROL_MATRIX = 1.0  # the wrong matrix must cost at least this much, and 4x the right one (measured 1.75)


def read_meta(p):
    with open(p, "r", encoding="utf-8") as f:
        return json.load(f)


def encode_case(name, extra, truth, expect_mode, premul, video_size, controls=True):
    out = path(name + ".webm")
    rc, so, se = cli("encode", path("src", "f_%04d.png"), "--out", out, "--fps", "30", "--crf", "10", "--gop", "30",
                     "--ffmpeg", FFDIR, "--quiet", *extra)
    check(rc == 0, f"encode {name}: exit code 0", f"rc={rc}\n{so}\n{se}")
    if rc != 0:
        return
    sidecar = path(name + ".acmeta.json")
    check(os.path.exists(out) and os.path.exists(sidecar) and not os.path.exists(out + ".part"),
          f"encode {name}: clip and sidecar exist, no .part left")
    meta = read_meta(sidecar)
    check(meta.get("schema") == 1 and meta.get("alpha_mode") == expect_mode and meta.get("premultiplied") is premul and
          meta.get("color_range") == "limited" and meta.get("matrix") == "bt601" and
          meta.get("content_size") == [W, H], f"encode {name}: sidecar content", meta)
    check("result: PASS (0 warnings)" in so, f"encode {name}: its own verify passed without warnings", so)

    rc, so, se = cli("inspect", out)
    info = kv(so)
    check(rc == 0 and info.get("playable") == "yes" and info.get("alpha_mode") == expect_mode and
          info.get("video_size") == video_size and info.get("content_size") == f"{W}x{H}" and
          info.get("premultiplied") == ("yes" if premul else "no") and info.get("metadata_schema") == "1" and
          info.get("seekable_at_packets") == "0,30" and info.get("cues", "").startswith("2 (all point"),
          f"inspect {name}", so)

    # A separate verify run, with the reference decode.
    rc, so, se = cli("verify", out, "--ffmpeg", FFDIR)
    check(rc == 0 and "result: PASS (0 warnings)" in so and "byte-identical" in so, f"verify {name}", so)

    dec = Decoded(out, sidecar, info)
    check(dec.n == NFRAMES, f"{name}: {NFRAMES} frames decode", dec.n)
    worst_abs, worst_edge, ctl_edge, ctl709 = 0.0, 0.0, 99.0, 0.0
    for k, fi in enumerate(TRUTH_FRAMES):
        rgb, alpha, pm = rgba_from_decoded(dec, fi, meta)
        a, e = composite_stats(rgb, alpha, pm, truth[k])
        worst_abs, worst_edge = max(worst_abs, a), max(worst_edge, abs(e))
        if controls:
            _, e2 = composite_stats(rgb, alpha, not pm, truth[k])
            ctl_edge = min(ctl_edge, abs(e2))
            rgb7, alpha7, pm7 = rgba_from_decoded(dec, fi, meta, matrix_override="bt709")
            a7, _ = composite_stats(rgb7, alpha7, pm7, truth[k])
            ctl709 = max(ctl709, a7)
    print(f"      {name}: mean abs error {worst_abs:.3f}, edge mean |{worst_edge:.3f}|"
          + (f"; controls: wrong flag edge |{ctl_edge:.2f}|, wrong matrix mean abs {ctl709:.2f}" if controls else ""))
    check(worst_abs <= MAX_MEAN_ABS, f"{name}: composite matches the source (mean abs error {worst_abs:.3f} <= {MAX_MEAN_ABS})")
    check(worst_edge <= MAX_EDGE_MEAN, f"{name}: soft edge has no dark or bright rim (|edge mean| {worst_edge:.3f} <= {MAX_EDGE_MEAN})")
    if controls:
        check(ctl_edge >= MIN_CONTROL_EDGE, f"{name}: control - the wrong premultiplied flag is caught ({ctl_edge:.2f} >= {MIN_CONTROL_EDGE})")
        check(ctl709 >= MIN_CONTROL_MATRIX and ctl709 > 4 * worst_abs,
              f"{name}: control - the wrong matrix is caught ({ctl709:.2f} >= {MIN_CONTROL_MATRIX} and > 4x {worst_abs:.2f})")
    return info


def part_b(truth):
    print("\n== B: encode in every mode")
    cases = [
        ("native_premul", ["--mode", "native_alpha"], "native_alpha", True, f"{W}x{H}"),
        ("native_straight", ["--mode", "native_alpha", "--straight"], "native_alpha", False, f"{W}x{H}"),
        ("packed_right_premul", ["--mode", "packed_right"], "packed_right", True, f"{2 * W}x{H}"),
        ("packed_right_straight", ["--mode", "packed_right", "--straight"], "packed_right", False, f"{2 * W}x{H}"),
        ("packed_bottom_premul", ["--mode", "packed_bottom"], "packed_bottom", True, f"{W}x{2 * H}"),
        ("packed_bottom_straight", ["--mode", "packed_bottom", "--straight"], "packed_bottom", False, f"{W}x{2 * H}"),
        ("auto_alpha", [], "native_alpha", True, f"{W}x{H}"),
    ]
    for name, extra, mode, premul, size in cases:
        encode_case(name, extra, truth, mode, premul, size)


# ---- C: options, inputs, refusals ----------------------------------------------------------------------------------

def picture_check(name, clip, truth, tolerance=MAX_MEAN_ABS):
    meta = read_meta(os.path.splitext(clip)[0] + ".acmeta.json")
    rc, so, se = cli("inspect", clip)
    info = kv(so)
    dec = Decoded(clip, os.path.splitext(clip)[0] + ".acmeta.json", info)
    worst, edge = 0.0, 0.0
    for k, fi in enumerate(TRUTH_FRAMES):
        rgb, alpha, pm = rgba_from_decoded(dec, fi, meta)
        a, e = composite_stats(rgb, alpha, pm, truth[k])
        worst, edge = max(worst, a), max(edge, abs(e))
    check(worst <= tolerance and edge <= MAX_EDGE_MEAN, f"{name}: picture matches the source (mean abs {worst:.3f}, |edge| {edge:.3f})")
    return dec, info


def inspect_of(clip, *extra):
    rc, so, se = cli("inspect", clip, *extra)
    return rc, kv(so), so


def part_c(truth):
    print("\n== C: inputs, options and refusals")
    seq = path("src", "f_%04d.png")
    common = ["--crf", "10", "--ffmpeg", FFDIR, "--quiet"]

    # Inputs other than a PNG sequence.
    for name, src, extra in (("from_mov", path("src.mov"), []), ("from_webm_alpha", path("in_alpha.webm"), [])):
        out = path(name + ".webm")
        rc, so, se = cli("encode", src, "--out", out, "--gop", "30", *common, *extra)
        check(rc == 0, f"encode {name}: exit code 0", f"{so}\n{se}")
        if rc == 0:
            meta = read_meta(path(name + ".acmeta.json"))
            check(meta["alpha_mode"] == "native_alpha" and meta["premultiplied"] is True, f"encode {name}: auto mode found the alpha", meta)
            check("result: PASS (0 warnings)" in so, f"encode {name}: verify passed without warnings", so)
            picture_check(name, out, truth, tolerance=1.5)
    out = path("from_rgb.webm")
    rc, so, se = cli("encode", path("src_rgb", "f_%04d.png"), "--out", out, "--fps", "30", *common)
    check(rc == 0, "encode of an opaque sequence: exit code 0", f"{so}\n{se}")
    if rc == 0:
        meta = read_meta(path("from_rgb.acmeta.json"))
        rc, info, so = inspect_of(out)
        check(meta["alpha_mode"] == "opaque" and meta["premultiplied"] is False and info.get("alpha_stream") == "no" and
              info.get("alpha_mode") == "opaque", "an opaque input becomes an opaque clip (auto mode)", meta)
    out = path("alpha_dropped.webm")
    rc, so, se = cli("encode", seq, "--out", out, "--fps", "30", "--mode", "opaque", *common)
    rc2, info, _ = inspect_of(out)
    check(rc == 0 and info.get("alpha_stream") == "no" and info.get("alpha_mode") == "opaque",
          "--mode opaque on footage with alpha drops the alpha", f"{so}\n{se}")

    # Options.
    for name, extra, want in (("gop10", ["--gop", "10"], "0,10,20,30,40,50"), ("gop_default", [], "0")):
        out = path(name + ".webm")
        rc, so, se = cli("encode", seq, "--out", out, "--fps", "30", *common, *extra)
        rc2, info, _ = inspect_of(out)
        check(rc == 0 and info.get("seekable_at_packets") == want and info.get("keyframes", "").split()[:1] != ["colour=0"],
              f"--gop: {name} puts the seek points at {want}", info.get("seekable_at_packets"))
    out = path("fps15.webm")
    rc, so, se = cli("encode", path("src.mov"), "--out", out, "--fps", "15", *common)
    rc2, info, _ = inspect_of(out)
    check(rc == 0 and info.get("packets") == "30" and info.get("frame_rate", "").startswith("15.000"),
          "--fps 15 resamples a 30 fps movie to 30 packets at 15 fps", f"{info.get('packets')} {info.get('frame_rate')}\n{so}\n{se}")
    out = path("seq15.webm")
    rc, so, se = cli("encode", seq, "--out", out, "--fps", "15", *common)
    rc2, info, _ = inspect_of(out)
    check(rc == 0 and info.get("packets") == "60" and info.get("frame_rate", "").startswith("15.000"),
          "--fps 15 on an image sequence is the input rate: 60 packets at 15 fps", f"{info.get('packets')} {info.get('frame_rate')}")

    for name, extra, matrix, rng in (("m709_full_native", ["--mode", "native_alpha", "--matrix", "bt709", "--range", "full"], "bt709", "full"),
                                     ("m709_full_packed", ["--mode", "packed_right", "--matrix", "bt709", "--range", "full"], "bt709", "full"),
                                     ("m601_limited_packed", ["--mode", "packed_right"], "bt601", "limited")):
        out = path(name + ".webm")
        rc, so, se = cli("encode", seq, "--out", out, "--fps", "30", "--gop", "30", "--loop", *common, *extra)
        check(rc == 0, f"encode {name}: exit code 0", f"{so}\n{se}")
        if rc != 0:
            continue
        meta = read_meta(path(name + ".acmeta.json"))
        check(meta["matrix"] == matrix and meta["color_range"] == rng and meta["loop_hint"] is True,
              f"encode {name}: sidecar says {matrix} / {rng} / loop", meta)
        dec, info = picture_check(name, out, truth)
        check(info.get("matrix") == matrix and info.get("color_range") == rng and info.get("loop_hint") == "yes", f"inspect {name}: the core reads the same", info)
        if meta["alpha_mode"] == "packed_right":
            y = dec.frame(20)[0][:, W:]
            lo, hi = (0, 255) if rng == "full" else (16, 235)
            check(abs(int(y.min()) - lo) <= 3 and abs(int(y.max()) - hi) <= 3,
                  f"encode {name}: the alpha image spans luma {lo}..{hi} (got {int(y.min())}..{int(y.max())})")

    # Non-ASCII paths, in and out.
    zh_in = path("素材")
    os.makedirs(zh_in)
    for i in range(12):
        shutil.copy(path("src", f"f_{i:04d}.png"), os.path.join(zh_in, f"帧_{i:04d}.png"))
    zh_out = path("成品")
    os.makedirs(zh_out)
    out = os.path.join(zh_out, "片段.webm")
    rc, so, se = cli("encode", os.path.join(zh_in, "帧_%04d.png"), "--out", out, "--fps", "30", *common)
    check(rc == 0 and os.path.exists(os.path.join(zh_out, "片段.acmeta.json")), "encode with non-ASCII input and output paths", f"{so}\n{se}")
    rc, info, so = inspect_of(out)
    check(rc == 0 and info.get("playable") == "yes" and info.get("sidecar", "").endswith("片段.acmeta.json (next to the clip)"),
          "inspect with a non-ASCII path finds the sidecar", so)
    rc, so, se = cli("verify", out, "--ffmpeg", FFDIR)
    check(rc == 0 and "byte-identical" in so, "verify with a non-ASCII path (also the reference decode)", so)

    # The output folder is created when it does not exist.
    out = path("new", "nested", "dir", "clip.webm")
    rc, so, se = cli("encode", path("src_rgb", "f_%04d.png"), "--out", out, "--fps", "30", *common)
    check(rc == 0 and os.path.exists(out) and os.path.exists(path("new", "nested", "dir", "clip.acmeta.json")),
          "encode creates the output folder", f"{so}\n{se}")

    # Overwriting.
    out = path("native_premul.webm")
    size_before = os.path.getsize(out)
    rc, so, se = cli("encode", seq, "--out", out, "--fps", "30", "--mode", "packed_right", *common)
    check(rc == 1 and "already exists" in se and os.path.getsize(out) == size_before, "an existing output is refused without --force and left alone", se)
    rc, so, se = cli("encode", seq, "--out", out, "--fps", "30", "--mode", "packed_right", "--force", *common)
    rc2, info, _ = inspect_of(out)
    check(rc == 0 and info.get("alpha_mode") == "packed_right" and read_meta(path("native_premul.acmeta.json"))["alpha_mode"] == "packed_right",
          "--force replaces the clip and its sidecar", f"{so}\n{se}")
    out = path("noverify.webm")
    rc, so, se = cli("encode", seq, "--out", out, "--fps", "30", "--no-verify", *common)
    check(rc == 0 and "result:" not in so and os.path.exists(out) and os.path.exists(path("noverify.acmeta.json")), "--no-verify writes the files without the check", so)

    # Refusals.
    def refuse(label, args, rc_want, text=None):
        rc, so, se = cli("encode", *args)
        ok = rc == rc_want and (text is None or text in se)
        check(ok, f"refused: {label}", f"rc={rc} stderr={se.strip()[:300]}")

    out = path("refused.webm")
    refuse("no --out", [seq, "--fps", "30"], 2)
    refuse("an input that does not exist", [path("nothing.mov"), "--out", out], 1, "does not exist")
    refuse("an image sequence without --fps", [seq, "--out", out, "--ffmpeg", FFDIR], 1, "needs --fps")
    refuse("a bad --mode", [seq, "--out", out, "--fps", "30", "--mode", "keyed", "--ffmpeg", FFDIR], 1, "--mode must be")
    refuse("a bad --matrix", [seq, "--out", out, "--fps", "30", "--matrix", "bt2020", "--ffmpeg", FFDIR], 1, "--matrix must be")
    refuse("--crf out of range", [seq, "--out", out, "--fps", "30", "--crf", "99"], 2)
    refuse("--gop 0", [seq, "--out", out, "--fps", "30", "--gop", "0"], 2)
    refuse("an unknown option", [seq, "--out", out, "--fps", "30", "--wibble"], 2)
    refuse("native alpha of footage without alpha", [path("src_rgb", "f_%04d.png"), "--out", out, "--fps", "30", "--mode", "native_alpha", "--ffmpeg", FFDIR],
           1, "no alpha channel")
    refuse("packed alpha of footage without alpha", [path("src_rgb", "f_%04d.png"), "--out", out, "--fps", "30", "--mode", "packed_right", "--ffmpeg", FFDIR],
           1, "no alpha channel")
    refuse("an odd size in a packed mode", [path("src_odd", "f_%04d.png"), "--out", out, "--fps", "30", "--mode", "packed_bottom", "--ffmpeg", FFDIR],
           1, "odd")
    refuse("a missing ffmpeg", [seq, "--out", out, "--fps", "30", "--ffmpeg", path("no_such_dir", "ffmpeg.exe")], 1, "ffmpeg")
    junk = path("junk.png")
    with open(junk, "wb") as f:
        f.write(b"this is not an image" * 10)
    refuse("an input ffprobe cannot read", [junk, "--out", out, "--ffmpeg", FFDIR], 1)
    check(not os.path.exists(out) and not os.path.exists(out + ".part"), "refused runs leave no output behind")

    # An odd-sized clip with alpha works in native mode.
    out = path("odd_native.webm")
    rc, so, se = cli("encode", path("src_odd", "f_%04d.png"), "--out", out, "--fps", "30", *common)
    rc2, info, _ = inspect_of(out)
    check(rc == 0 and info.get("video_size") == "321x181" and "result: PASS (0 warnings)" in so, "an odd-sized (321x181) clip with alpha encodes and verifies", f"{so}\n{se}")


# ---- D: verify and inspect on hand-made files --------------------------------------------------------------------

def ebml_size(n):
    for length in range(1, 9):
        if n < (1 << (7 * length)) - 1:
            return (n | (1 << (7 * length))).to_bytes(length, "big")
    raise ValueError("too large")


def el(id_hex, payload):
    return bytes.fromhex(id_hex) + ebml_size(len(payload)) + payload


def uint(n):
    return n.to_bytes(max(1, (n.bit_length() + 7) // 8), "big")


def read_ivf(p):
    with open(p, "rb") as f:
        data = f.read()
    assert data[:4] == b"DKIF"
    hdr = struct.unpack("<H", data[6:8])[0]
    pos, packets = hdr, []
    while pos + 12 <= len(data):
        size = struct.unpack("<I", data[pos:pos + 4])[0]
        packets.append(data[pos + 12:pos + 12 + size])
        pos += 12 + size
    return packets


def mux_webm(p, w, h, color, alpha, fps=30, cues="keys", color_gop=30, cluster_every=30, pts_ms=None,
             cue_rel_delta=0, cue_cluster_delta=0, alpha_flag=True):
    """A minimal WebM: one video track, one cluster per `cluster_every` packets, the alpha stream (if any) as
    BlockAdditions with BlockAddID 1, and Cues. `cues`: 'keys' (at every colour keyframe), None (no Cues element) or a
    list of packet indexes."""
    n = len(color)
    pts = pts_ms or [round(i * 1000 / fps) for i in range(n)]
    info = el("1549A966", el("2AD7B1", uint(1000000)) + el("4489", struct.pack(">d", float(pts[-1] + round(1000 / fps)))) +
              el("4D80", b"test") + el("5741", b"test"))
    video = el("B0", uint(w)) + el("BA", uint(h)) + (el("53C0", uint(1)) if (alpha is not None and alpha_flag) else b"")
    tracks = el("1654AE6B", el("AE", el("D7", uint(1)) + el("73C5", uint(1)) + el("83", uint(1)) + el("86", b"V_VP9") +
                               el("23E383", uint(round(1e9 / fps))) + el("E0", video)))
    clusters, positions, rel_offsets = [], {}, {}
    seg_pos = len(info) + len(tracks)
    for start in range(0, n, cluster_every):
        tc = pts[start]
        body = el("E7", uint(tc))
        for i in range(start, min(n, start + cluster_every)):
            block = bytes([0x81]) + struct.pack(">h", pts[i] - tc) + bytes([0]) + color[i]
            if alpha is not None:
                group = el("A0", el("A1", block) + el("75A1", el("A6", el("EE", uint(1)) + el("A5", alpha[i]))))
            else:
                group = el("A0", el("A1", block))
            positions[i], rel_offsets[i] = seg_pos, len(body)
            body += group
        cl = el("1F43B675", body)
        clusters.append(cl)
        seg_pos += len(cl)
    cue_el = b""
    if cues is not None:
        idx = [i for i in range(n) if i % color_gop == 0] if cues == "keys" else list(cues)
        points = b"".join(el("BB", el("B3", uint(pts[i])) + el("B7", el("F7", uint(1)) + el("F1", uint(positions[i] + cue_cluster_delta)) +
                                                          el("F0", uint(rel_offsets[i] + cue_rel_delta)))) for i in idx)
        cue_el = el("1C53BB6B", points)
    ebml = el("1A45DFA3", el("4286", uint(1)) + el("42F7", uint(1)) + el("42F2", uint(4)) + el("42F3", uint(8)) +
              el("4282", b"webm") + el("4287", uint(4)) + el("4285", uint(2)))
    with open(p, "wb") as f:
        f.write(ebml + el("18538067", info + tracks + b"".join(clusters) + cue_el))


def vp9_ivf(out, inputs, vf, gop, crf=20, frames=NFRAMES):
    ffmpeg(*inputs, "-vf", vf, "-frames:v", str(frames), "-c:v", "libvpx-vp9", "-b:v", "0", "-crf", str(crf), "-g", str(gop),
           "-keyint_min", str(gop), "-auto-alt-ref", "0", "-lag-in-frames", "0", "-pix_fmt", "yuv420p", "-threads", "1", "-f", "ivf", out)


def part_d(truth):
    print("\n== D: verify / inspect on hand-made files")
    seq = ["-framerate", str(FPS), "-i", path("src", "f_%04d.png")]
    color_vf = "format=gbrap,premultiply=inplace=1,scale=out_color_matrix=bt601:out_range=tv,format=yuv420p"
    alpha_vf = "format=gbrap,alphaextract,scale=in_range=pc:out_range=pc,format=yuv420p"
    vp9_ivf(path("c30.ivf"), seq, color_vf, 30)
    vp9_ivf(path("a30.ivf"), seq, alpha_vf, 30)
    vp9_ivf(path("a20.ivf"), seq, alpha_vf, 20)
    vp9_ivf(path("a_opaque.ivf"), ["-f", "lavfi", "-i", f"color=c=white:s={W}x{H}:r={FPS}"], "format=gray,scale=in_range=pc:out_range=pc,format=yuv420p", 30)
    color, a30, a20, a255 = (read_ivf(path(n + ".ivf")) for n in ("c30", "a30", "a20", "a_opaque"))
    check(len(color) == len(a30) == len(a20) == len(a255) == NFRAMES, f"hand-made streams: {NFRAMES} packets each")

    sidecar = {"schema": 1, "alpha_mode": "native_alpha", "premultiplied": True, "color_range": "limited", "matrix": "bt601",
               "content_size": [W, H], "loop_hint": False}

    def make(name, alpha, **kw):
        mux_webm(path(name + ".webm"), W, H, kw.pop("color", color), alpha, **kw)
        with open(path(name + ".acmeta.json"), "w", encoding="utf-8") as f:
            json.dump(sidecar, f)
        return path(name + ".webm")

    def verify(clip, *extra):
        rc, so, se = cli("verify", clip, "--ffmpeg", FFDIR, *extra)
        return rc, verify_lines(so), so

    # The muxer itself: aligned streams must be a clean pass, including the reference decode of the hand-made alpha.
    clip = make("aligned", a30)
    rc, rep, so = verify(clip)
    check(rc == 0 and "result: PASS (0 warnings)" in so and "byte-identical" in so, "aligned colour and alpha keyframes: clean pass", so)

    # R10: keyframes that do not line up (colour every 30 frames, alpha every 20).
    clip = make("misaligned", a20)
    rc, rep, so = verify(clip)
    check(rc == 1 and tag_of(rep, "cues") == "FAIL" and "alpha stream" in rep["cues"][0][1], "R10: a cue at a colour keyframe that is no alpha keyframe fails", so)
    check(tag_of(rep, "keyframes") == "warn" and "only one of the two streams" in rep["keyframes"][0][1], "R10: the one-sided keyframes are reported", so)
    check(tag_of(rep, "decode") == "ok" and tag_of(rep, "seek") == "ok" and tag_of(rep, "ffmpeg") == "ok",
          "R10: the core still decodes it and seeks correctly (it falls back to the last point where both are keyframes)", so)
    rc, info, so = inspect_of(clip)
    check(info.get("keyframes") == "colour=2 alpha=3 seekable=1" and info.get("seekable_at_packets") == "0" and
          info.get("seek_granularity_frames") == str(NFRAMES) and "1 invalid" in info.get("cues", ""), "R10: inspect shows the misalignment", so)
    clip = make("misaligned_cue0", a20, cues=[0])
    rc, rep, so = verify(clip)
    check(rc == 0 and "result: PASS (1 warning)" in so and tag_of(rep, "keyframes") == "warn", "misaligned streams with a Cue only where both are keyframes: pass with the keyframe warning", so)

    # R9: Cues.
    clip = make("no_cues", a30, cues=None)
    rc, rep, so = verify(clip)
    check(rc == 1 and tag_of(rep, "cues") == "FAIL" and "no Cues" in rep["cues"][0][1] and tag_of(rep, "decode") == "ok", "R9: a file without Cues fails", so)
    rc, info, so = inspect_of(clip)
    check(info.get("cues") == "none", "inspect: cues none", so)
    for name, kw, text in (("cue_rel", dict(cue_rel_delta=1), "CueRelativePosition"),
                           ("cue_cluster", dict(cue_cluster_delta=1), "not the start of a cluster"),
                           ("cue_nonkey", dict(cues=[0, 10]), "not a keyframe")):
        clip = make(name, a30, **kw)
        rc, rep, so = verify(clip)
        check(rc == 1 and tag_of(rep, "cues") == "FAIL" and text in rep["cues"][0][1], f"R9: a wrong cue ({name}) fails: {text}", so)

    # Timestamps, corrupt data, constant alpha.
    pts = [round(i * 1000 / FPS) for i in range(NFRAMES)]
    pts[5] = pts[4]
    clip = make("dup_pts", a30, pts_ms=pts)
    rc, rep, so = verify(clip)
    check(rc == 1 and tag_of(rep, "timestamps") == "FAIL" and "not later than" in rep["timestamps"][0][1], "equal timestamps fail", so)
    bad = list(color)
    bad[35] = bad[35][:len(bad[35]) // 3]
    clip = make("corrupt", a30, color=bad)
    rc, rep, so = verify(clip)
    check(rc == 1 and tag_of(rep, "decode") == "FAIL" and "after 35 frames" in rep["decode"][0][1] and tag_of(rep, "seek") == "skip",
          "a corrupt packet: the decode fails at the right frame", so)
    clip = make("constant_alpha", a255)
    rc, rep, so = verify(clip)
    check(rc == 0 and tag_of(rep, "alpha") == "warn" and "255 in every pixel" in rep["alpha"][0][1], "an alpha that is 255 everywhere is reported", so)

    # The premultiplied flag against the picture.
    common = ["--fps", "30", "--crf", "10", "--gop", "30", "--ffmpeg", FFDIR, "--quiet", "--no-verify"]
    for name, extra, flag, want in (("flag_a", [], False, "not premultiplied"), ("flag_b", ["--straight"], True, "looks wrong")):
        out = path(name + ".webm")
        rc, so, se = cli("encode", path("src", "f_%04d.png"), "--out", out, *common, *extra)
        meta = read_meta(path(name + ".acmeta.json"))
        meta["premultiplied"] = flag
        wrong = path(name + "_wrong.acmeta.json")
        with open(wrong, "w", encoding="utf-8") as f:
            json.dump(meta, f)
        rc, rep, so = verify(out, "--meta", wrong)
        check(rc == 0 and tag_of(rep, "alpha") == "ok" and any(t == "warn" and want in s for t, s in rep["alpha"]),
              f"the premultiplied flag that contradicts the picture is reported ({want})", so)
        rc, rep, so = verify(out)
        check(rc == 0 and "result: PASS (0 warnings)" in so, f"... and the right flag is not ({name})", so)

    # Other ways a file can be wrong, and the sidecar.
    rc, so, se = cli("verify", path("no_such_file.webm"), "--no-ffmpeg")
    check(rc == 1 and "[FAIL] file" in so, "verify: a missing file fails", so)
    junk = path("junk.webm")
    with open(junk, "wb") as f:
        f.write(bytes(range(256)) * 20)
    rc, so, se = cli("verify", junk, "--no-ffmpeg")
    check(rc == 1 and "[FAIL] container" in so, "verify: random bytes fail as a container", so)
    rc, so, se = cli("inspect", junk)
    check(rc == 1 and "error" in se, "inspect: random bytes are an error", se)
    keyed = path("keyed.acmeta.json")
    with open(keyed, "w", encoding="utf-8") as f:
        json.dump(dict(sidecar, alpha_mode="keyed"), f)
    rc, rep, so = verify(path("aligned.webm"), "--meta", keyed)
    check(rc == 1 and tag_of(rep, "open") == "FAIL" and "keyed" in rep["open"][0][1], "a sidecar the core cannot honour fails the open check", so)
    rc, info, so = inspect_of(path("aligned.webm"), "--meta", keyed)
    check(rc == 0 and info.get("playable", "").startswith("no (") and "keyed" in info["playable"], "inspect: playable: no (... keyed ...)", so)
    # Without a sidecar the core cannot know that the colour is premultiplied: the clip is read as straight, and the
    # picture check says so (the one warning below).
    rc, rep, so = verify(path("aligned.webm"), "--no-meta")
    check(rc == 0 and "result: PASS (1 warning)" in so and tag_of(rep, "sidecar") == "missing" and
          any(t == "warn" and "read as not premultiplied" in s for t, s in rep.get("alpha", [])),
          "--no-meta: opened in auto mode, no sidecar warning, but the premultiplied colour is noticed", so)
    thin = path("thin.acmeta.json")
    with open(thin, "w", encoding="utf-8") as f:
        json.dump({"schema": 1, "alpha_mode": "native_alpha", "premultiplied": True}, f)
    rc, rep, so = verify(path("aligned.webm"), "--meta", thin)
    check(rc == 0 and tag_of(rep, "sidecar") == "warn" and "color_range" in rep["sidecar"][0][1], "a sidecar without matrix / range is a warning", so)
    shutil.copy(path("aligned.webm"), path("nosidecar.webm"))
    rc, rep, so = verify(path("nosidecar.webm"))
    check(rc == 0 and "result: PASS (2 warnings)" in so and tag_of(rep, "sidecar") == "warn",
          "a clip without a sidecar passes with the sidecar warning (and the premultiplied one)", so)

    # Where the reference decoder comes from.
    rc, rep, so = verify(path("aligned.webm"), "--no-meta")
    rc, so, se = cli("verify", path("aligned.webm"), "--no-ffmpeg")
    check(rc == 0 and "[skip] ffmpeg: not requested" in so, "--no-ffmpeg skips the reference decode", so)
    rc, so, se = cli("verify", path("aligned.webm"), "--ffmpeg", path("no_such_dir"))
    check(rc == 1 and "[FAIL] ffmpeg" in so, "--ffmpeg that cannot be started fails the check", so)
    env = dict(os.environ)
    env.pop("ALPHACLIP_FFMPEG", None)
    env["PATH"] = WORK  # no ffmpeg anywhere on PATH
    rc, so, se = run([CLI, "verify", path("aligned.webm")], env=env)
    check(rc == 0 and "[skip] ffmpeg" in so, "no ffmpeg found and none asked for: the check is skipped, not failed", so)
    env["ALPHACLIP_FFMPEG"] = FFDIR
    rc, so, se = run([CLI, "verify", path("aligned.webm")], env=env)
    check(rc == 0 and "byte-identical" in so, "ALPHACLIP_FFMPEG names the reference decoder", so)


# ---- E: straight colour with black under the transparent pixels ------------------------------------------------------

def make_hard_sources():
    """The same discs with a 1.5 px alpha ramp instead of 14 / 10 px (an anti-aliased cut-out, as a sprite or a UI
    element has). Returns the exact RGBA truth of the frames the checks look at. Two sequences: the colour continues
    under the transparent pixels (src_hard) or is black there (src_hard_black)."""
    d1 = "clip((62-hypot(X-160-60*sin(2*T),Y-90-18*cos(2*T)))*255/1.5,0,255)"
    d2 = "clip((22-hypot(X-70-30*cos(3*T),Y-135))*255/1.5,0,255)"
    alpha_e = f"max({d1},{d2})"
    base = f"nullsrc=s={W}x{H}:r={FPS}:d={NFRAMES / FPS}"
    alpha = f"{base},format=gray,geq=lum='{alpha_e}'"
    cont = f"{base},format=gbrp,geq=r='{RED}':g='{GREEN}':b='{BLUE}'"
    black = f"{base},format=gbrp,geq=r='({RED})*gt({alpha_e},0)':g='({GREEN})*gt({alpha_e},0)':b='({BLUE})*gt({alpha_e},0)'"
    for name, color in (("src_hard", cont), ("src_hard_black", black)):
        os.makedirs(path(name), exist_ok=True)
        ffmpeg("-lavfi", f"{color}[c];{alpha}[a];[c][a]alphamerge,format=rgba", "-start_number", "0", "-frames:v", str(NFRAMES),
               path(name, "f_%04d.png"))
    sel = "+".join(f"eq(n,{n})" for n in TRUTH_FRAMES)
    ffmpeg("-framerate", str(FPS), "-i", path("src_hard", "f_%04d.png"), "-vf", f"select='{sel}'", "-fps_mode", "passthrough",
           "-f", "rawvideo", "-pix_fmt", "rgba", path("truth_hard.raw"))
    return np.fromfile(path("truth_hard.raw"), dtype=np.uint8).reshape(len(TRUTH_FRAMES), H, W, 4)


def magnified_edge(rgb, alpha, premul, truth):
    """Signed mean error on the soft edge when the picture is shown at 2x with bilinear filtering of the stored planes (a
    GPU does that to a video that is scaled up), against the correct magnification: interpolate the PREMULTIPLIED truth."""
    t_a = truth[..., 3].astype(np.float32) / 255.0
    t_prem = truth[..., :3].astype(np.float32) * t_a[..., None]
    h, w = alpha.shape

    def mag(p):
        return upsample(p, 2 * h, 2 * w)

    r2 = np.stack([mag(rgb[..., c]) for c in range(3)], -1)
    a2 = mag(alpha)
    tp2 = np.stack([mag(t_prem[..., c]) for c in range(3)], -1)
    ta2 = mag(t_a)
    edge = (ta2 > 0.05) & (ta2 < 0.95)
    errs = []
    for bg in BACKGROUNDS:
        b = np.array(bg, np.float32)
        out = (r2 if premul else r2 * a2[..., None]) + b * (1 - a2[..., None])
        ref = tp2 + b * (1 - ta2[..., None])
        errs.append((out - ref)[edge].mean())
    return float(np.mean(errs))


def part_e(truth):
    """Closes the M4 gap 'straight colour with black under transparent pixels', and shows why `encode` multiplies the
    colour by alpha by default: at 1:1 such footage is fine, but when the video is scaled up the bilinear filter blends
    the black of the transparent pixels into the edge pixels."""
    print("\n== E: straight footage with black under the transparent pixels")
    common = ["--fps", "30", "--crf", "10", "--gop", "30", "--ffmpeg", FFDIR, "--quiet", "--no-verify"]
    stats = {}
    truth_hard = make_hard_sources()
    for name, src, extra, tr in (("cont_straight", path("src", "f_%04d.png"), ["--straight"], truth),
                                 ("black_straight", path("src_black", "f_%04d.png"), ["--straight"], truth),
                                 ("black_premul", path("src_black", "f_%04d.png"), [], truth),
                                 ("hard_cont_straight", path("src_hard", "f_%04d.png"), ["--straight"], truth_hard),
                                 ("hard_cont_premul", path("src_hard", "f_%04d.png"), [], truth_hard),
                                 ("hard_black_straight", path("src_hard_black", "f_%04d.png"), ["--straight"], truth_hard),
                                 ("hard_black_premul", path("src_hard_black", "f_%04d.png"), [], truth_hard)):
        out = path(name + ".webm")
        rc, so, se = cli("encode", src, "--out", out, *common, *extra)
        check(rc == 0, f"encode {name}: exit code 0", so + "\n" + se)
        meta = read_meta(path(name + ".acmeta.json"))
        info = inspect_of(out)[1]
        dec = Decoded(out, path(name + ".acmeta.json"), info)
        abs1, edge1, edge2, signed2 = 0.0, 0.0, 0.0, 0.0
        for k, fi in enumerate(TRUTH_FRAMES):
            rgb, alpha, pm = rgba_from_decoded(dec, fi, meta)
            a, e = composite_stats(rgb, alpha, pm, tr[k])
            m = magnified_edge(rgb, alpha, pm, tr[k])
            abs1, edge1 = max(abs1, a), max(edge1, abs(e))
            edge2 = max(edge2, abs(m))
            signed2 += m / len(TRUTH_FRAMES)
        stats[name] = (abs1, edge1, edge2, signed2)
        print(f"      {name}: at 1:1 mean abs {abs1:.3f}, |edge mean| {edge1:.3f}; magnified 2x |edge mean| {edge2:.3f} "
              f"(signed {signed2:+.3f}; negative = too dark)")
    # Soft edges (the M4 shape: 14 / 10 px ramps): every variant is clean, at 1:1 and magnified.
    for name in ("cont_straight", "black_straight", "black_premul"):
        a, e1, e2, _ = stats[name]
        check(a <= MAX_MEAN_ABS and e1 <= MAX_EDGE_MEAN and e2 <= MAX_EDGE_MEAN, f"soft edges, {name}: clean at 1:1 and magnified 2x")
    # Tight edges (1.5 px ramp). Measured: straight colour that continues under the transparent pixels is the most
    # accurate; premultiplied is a little bright (and identical whatever the source had under the transparent pixels);
    # straight with black under the transparent pixels gets a dark rim when magnified.
    cs, bs, pc = stats["hard_cont_straight"], stats["hard_black_straight"], stats["hard_black_premul"]
    check(cs[2] <= MAX_EDGE_MEAN, f"tight edges: straight colour that continues under the transparent pixels is accurate even magnified ({cs[2]:.3f})")
    check(bs[3] <= -MAX_EDGE_MEAN, f"tight edges: straight colour with black under the transparent pixels gets a dark rim when magnified (signed {bs[3]:+.2f} <= -{MAX_EDGE_MEAN})")
    check(abs(stats["hard_cont_premul"][2] - pc[2]) < 0.05, "tight edges: premultiplying erases the difference between the two sources")
    check(pc[2] < abs(bs[3]) and pc[3] > 0, f"tight edges: premultiplied colour does not darken the edge ({pc[3]:+.2f}) and is closer than the black-filled straight one ({pc[2]:.2f} < {abs(bs[3]):.2f})")
    check(cs[2] < pc[2], f"tight edges: ... but straight colour that continues under the transparent pixels beats it ({cs[2]:.3f} < {pc[2]:.2f}); `encode` cannot create that colour, it only keeps what the footage has")
    for name, want_warning in (("black_straight", True), ("hard_black_straight", True), ("black_premul", False),
                               ("cont_straight", False), ("hard_cont_straight", False)):
        rc, so, se = cli("verify", path(name + ".webm"), "--no-ffmpeg")
        rep = verify_lines(so)
        warned = any(t == "warn" and "dark rim" in x for t, x in rep.get("alpha", []))
        check(rc == 0 and warned == want_warning and (want_warning or "result: PASS (0 warnings)" in so),
              f"verify {'warns' if want_warning else 'does not warn'} about {name}", so)


def main():
    global CLI, FFDIR, FFMPEG, WORK
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--ffmpeg-dir", required=True)
    ap.add_argument("--workdir", required=True, help="an empty folder")
    ap.add_argument("--truth", help="samples/m4/truth_rgba_3f.raw: the source must reproduce it exactly")
    ap.add_argument("--only", default="", help="run only these parts, e.g. AB")
    args = ap.parse_args()
    CLI = os.path.abspath(args.cli)
    FFDIR = os.path.abspath(args.ffmpeg_dir)
    FFMPEG = os.path.join(FFDIR, "ffmpeg.exe" if os.name == "nt" else "ffmpeg")
    WORK = os.path.abspath(args.workdir)
    os.makedirs(WORK, exist_ok=True)
    if os.listdir(WORK):
        print("error: --workdir must be empty", file=sys.stderr)
        return 2
    only = args.only.upper()

    print("== A: footage")
    truth = make_sources()
    if args.truth:
        ref = np.fromfile(args.truth, dtype=np.uint8)
        check(ref.size == truth.size and np.array_equal(ref.reshape(truth.shape), truth),
              "the generated PNG sequence reproduces samples/m4/truth_rgba_3f.raw exactly")
    check(len(os.listdir(path("src"))) == NFRAMES, f"{NFRAMES} source frames")

    if not only or "B" in only:
        part_b(truth)
    if not only or "C" in only:
        part_c(truth)
    if not only or "D" in only:
        part_d(truth)
    if not only or "E" in only:
        part_e(truth)
    print(f"\n{checks - failures}/{checks} checks passed")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
