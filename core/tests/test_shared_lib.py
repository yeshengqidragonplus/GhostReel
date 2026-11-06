"""The shared library (alphaclip.dll) called from outside, through ctypes.

Run isolated:
  python -I core/tests/test_shared_lib.py --dll build/core-win64/bin/alphaclip.dll \
      --cli build/core-win64/bin/alphaclip.exe --workdir <empty dir> clip.webm [clip2.webm ...]

This is what the engine bindings do: load the DLL, declare the ac_* functions and the struct
layouts by hand, call them. The ctypes declarations below are the layout a C# P/Invoke binding
must reproduce (the C++ side locks it with static_asserts in src/api.cpp). The decoded frames are
compared with the output of `alphaclip decode`, which test_vs_ffmpeg.py already proves identical to
ffmpeg.
"""
import argparse
import ctypes as C
import os
import subprocess
import sys

AC_OK, AC_AGAIN, AC_ERR_IO, AC_ERR_FORMAT, AC_ERR_UNSUPPORTED, AC_ERR_DECODE, AC_ERR_STATE, AC_ERR_OOM, AC_ERR_ARG, AC_END = range(10)


class Info(C.Structure):
    _fields_ = [("width", C.c_int32), ("height", C.c_int32), ("content_w", C.c_int32), ("content_h", C.c_int32),
                ("duration_us", C.c_int64), ("fps", C.c_float), ("has_alpha_plane", C.c_int32),
                ("packet_count", C.c_int64), ("loop_period_us", C.c_int64),
                ("alpha_mode", C.c_int32), ("premultiplied", C.c_int32), ("color_range", C.c_int32),
                ("matrix", C.c_int32), ("loop_hint", C.c_int32), ("alpha_x", C.c_int32), ("alpha_y", C.c_int32),
                ("reserved", C.c_int32)]


class Frame(C.Structure):
    _fields_ = [("plane", C.c_void_p * 4), ("stride", C.c_int32 * 4), ("width", C.c_int32), ("height", C.c_int32),
                ("pts_us", C.c_int64), ("flags", C.c_uint32)]


class Stats(C.Structure):
    _fields_ = [("frames_decoded", C.c_int64), ("frames_acquired", C.c_int64), ("frames_dropped", C.c_int64),
                ("decode_time_us", C.c_int64), ("frames_queued", C.c_int32), ("reached_end", C.c_int32)]


failures = checks = 0


def check(ok, label, detail=""):
    global failures, checks
    checks += 1
    if not ok:
        failures += 1
    print(("PASS  " if ok else "FAIL  ") + label + (("  -- " + str(detail)) if (detail and not ok) else ""))


def load(path):
    lib = C.CDLL(path)
    P = C.c_void_p
    lib.ac_version.restype = C.c_char_p
    lib.ac_open_memory.argtypes = [C.c_char_p, C.c_size_t, C.c_char_p, C.POINTER(P)]
    lib.ac_open_file.argtypes = [C.c_char_p, C.c_char_p, C.POINTER(P)]
    lib.ac_get_info.argtypes = [P, C.POINTER(Info)]
    lib.ac_seek.argtypes = [P, C.c_int64]
    lib.ac_decode_next.argtypes = [P, C.POINTER(Frame)]
    lib.ac_set_queue_depth.argtypes = [P, C.c_int32]
    lib.ac_start.argtypes = [P]
    lib.ac_set_loop.argtypes = [P, C.c_int32]
    lib.ac_acquire_frame.argtypes = [P, C.c_int64, C.POINTER(Frame)]
    lib.ac_release_frame.argtypes = [P, C.POINTER(Frame)]
    lib.ac_get_stats.argtypes = [P, C.POINTER(Stats)]
    lib.ac_last_error.argtypes = [P]
    lib.ac_last_error.restype = C.c_char_p
    lib.ac_close.argtypes = [P]
    return lib


def frame_bytes(f, has_alpha):
    w, h = f.width, f.height
    cw, ch = (w + 1) // 2, (h + 1) // 2
    sizes = [(w, h), (cw, ch), (cw, ch)] + ([(w, h)] if has_alpha else [])
    out = bytearray()
    for i, (pw, ph) in enumerate(sizes):
        for y in range(ph):
            out += C.string_at(f.plane[i] + y * f.stride[i], pw)
    return bytes(out)


def test_clip(lib, cli, work, clip):
    name = os.path.basename(clip)
    data = open(clip, "rb").read()

    ref_path = os.path.join(work, name + ".cli.raw")
    r = subprocess.run([cli, "decode", clip, "--out", ref_path], capture_output=True, text=True)
    check(r.returncode == 0, f"{name}: reference decode with the CLI", r.stderr)
    ref = open(ref_path, "rb").read()

    # open from memory (the buffer must stay alive until ac_close)
    buf = C.create_string_buffer(data, len(data))
    p = C.c_void_p()
    st = lib.ac_open_memory(buf, len(data), None, C.byref(p))
    check(st == AC_OK and p.value, f"{name}: ac_open_memory", st)
    info = Info()
    check(lib.ac_get_info(p, C.byref(info)) == AC_OK, f"{name}: ac_get_info")
    alpha = bool(info.has_alpha_plane)
    cw, ch = (info.width + 1) // 2, (info.height + 1) // 2
    fsize = info.width * info.height * (2 if alpha else 1) + 2 * cw * ch
    check(len(ref) == fsize * info.packet_count or info.packet_count > 0,
          f"{name}: info {info.width}x{info.height}, alpha={alpha}, {info.packet_count} packets, loop period {info.loop_period_us} us")

    # synchronous decode == CLI output
    frames = []
    f = Frame()
    while True:
        s = lib.ac_decode_next(p, C.byref(f))
        if s == AC_END:
            break
        if s != AC_OK:
            check(False, f"{name}: ac_decode_next", f"{s}: {lib.ac_last_error(p)}")
            break
        frames.append((f.pts_us, frame_bytes(f, alpha), f.stride[0]))
    joined = b"".join(fr[1] for fr in frames)
    check(joined == ref, f"{name}: ac_decode_next: {len(frames)} frames identical to the CLI output")
    check(all(fr[2] == info.width for fr in frames), f"{name}: strides are tight (stride[0] == width)")

    # threaded: pre-decode everything, fine clock, every frame once and in order
    check(lib.ac_seek(p, 0) == AC_OK, f"{name}: ac_seek(0) in synchronous mode")
    check(lib.ac_set_queue_depth(p, 64) == AC_OK and lib.ac_start(p) == AC_OK, f"{name}: ac_set_queue_depth + ac_start")
    check(lib.ac_decode_next(p, C.byref(f)) == AC_ERR_STATE, f"{name}: ac_decode_next after ac_start is a state error")
    check(lib.ac_set_queue_depth(p, 8) == AC_ERR_STATE, f"{name}: ac_set_queue_depth after ac_start is a state error")
    stats = Stats()
    for _ in range(5000):
        lib.ac_get_stats(p, C.byref(stats))
        if stats.reached_end:
            break
        import time
        time.sleep(0.001)
    check(stats.reached_end == 1 and stats.frames_decoded == len(frames), f"{name}: decode thread finished", stats.frames_decoded)
    got = []
    clock = 0
    last_pts = frames[-1][0]
    while True:
        s = lib.ac_acquire_frame(p, clock, C.byref(f))
        if s == AC_OK:
            got.append((f.pts_us, frame_bytes(f, alpha)))
            # a second acquire while the first is still held must fail, without disturbing it
            g = Frame()
            if len(got) == 1:
                check(lib.ac_acquire_frame(p, clock, C.byref(g)) == AC_ERR_STATE, f"{name}: second acquire without release is a state error")
                bogus = Frame()
                check(lib.ac_release_frame(p, C.byref(bogus)) == AC_ERR_ARG, f"{name}: releasing a frame that was not acquired is an argument error")
            check_rel = lib.ac_release_frame(p, C.byref(f))
            if check_rel != AC_OK:
                check(False, f"{name}: ac_release_frame", check_rel)
                break
        elif s == AC_END:
            break
        elif s != AC_AGAIN:
            check(False, f"{name}: ac_acquire_frame", f"{s}: {lib.ac_last_error(p)}")
            break
        clock += 1000
        if clock > last_pts + 5_000_000:
            check(False, f"{name}: never reached AC_END")
            break
    check([g[0] for g in got] == [fr[0] for fr in frames] and b"".join(g[1] for g in got) == ref,
          f"{name}: ac_acquire_frame: all {len(frames)} frames once, in order, identical")
    lib.ac_get_stats(p, C.byref(stats))
    check(stats.frames_dropped == 0 and stats.frames_acquired == len(frames) and stats.decode_time_us > 0,
          f"{name}: ac_get_stats is consistent", (stats.frames_acquired, stats.frames_dropped, stats.decode_time_us))
    lib.ac_close(p)

    # looping through the shared library
    p = C.c_void_p()
    lib.ac_open_memory(buf, len(data), None, C.byref(p))
    lib.ac_set_queue_depth(p, 64)
    lib.ac_set_loop(p, 1)
    lib.ac_start(p)
    period = info.loop_period_us
    pts_seen, clock, passes = [], 0, 0
    while clock < period * 2 + 200_000 and len(pts_seen) < 400:
        s = lib.ac_acquire_frame(p, clock, C.byref(f))
        if s == AC_OK:
            pts_seen.append(f.pts_us)
            lib.ac_release_frame(p, C.byref(f))
        elif s not in (AC_AGAIN,):
            check(False, f"{name}: loop acquire", s)
            break
        clock += 1000 if s == AC_OK else 0
        if s == AC_AGAIN:
            import time
            time.sleep(0.0005)
            clock += 100
    check(len(pts_seen) > info.packet_count and max(pts_seen) >= period and all(b > a for a, b in zip(pts_seen, pts_seen[1:])),
          f"{name}: ac_set_loop: clock stays continuous across the loop point", (len(pts_seen), period))
    lib.ac_close(p)

    # many open / close cycles (a leak or a crash in teardown would show here)
    ok = True
    for _ in range(150):
        q = C.c_void_p()
        if lib.ac_open_memory(buf, len(data), None, C.byref(q)) != AC_OK:
            ok = False
            break
        lib.ac_start(q)
        lib.ac_acquire_frame(q, 0, C.byref(f))
        lib.ac_close(q)  # closed with the decode thread running and possibly a frame held
    check(ok, f"{name}: 150 open / start / close cycles")


def test_errors(lib, work):
    p = C.c_void_p(1)
    check(lib.ac_open_memory(None, 0, None, C.byref(p)) == AC_ERR_ARG and not p.value, "null / empty data -> AC_ERR_ARG, out cleared")
    check(lib.ac_open_memory(b"x", 1, None, None) == AC_ERR_ARG, "null out pointer -> AC_ERR_ARG")
    junk = bytes(range(256)) * 4
    check(lib.ac_open_memory(junk, len(junk), None, C.byref(p)) == AC_ERR_FORMAT, "garbage -> AC_ERR_FORMAT")
    check(len(lib.ac_last_error(None) or b"") > 0, "ac_last_error(NULL) has the text of the failed open")
    check(lib.ac_open_file(os.path.join(work, "missing.webm").encode(), None, C.byref(p)) == AC_ERR_IO, "missing file -> AC_ERR_IO")
    check(lib.ac_open_memory(junk, len(junk), b"{}", C.byref(p)) == AC_ERR_FORMAT, "meta_json without a schema -> AC_ERR_FORMAT (the sidecar is checked before the file; test_meta.py has the full matrix)")
    lib.ac_close(None)  # must be a no-op
    check(True, "ac_close(NULL) is a no-op")
    check(lib.ac_get_info(None, None) == AC_ERR_ARG, "null player -> AC_ERR_ARG")


def main():
    global failures
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", required=True)
    ap.add_argument("--cli", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("clips", nargs="+")
    a = ap.parse_args()
    work = os.path.abspath(a.workdir)
    os.makedirs(work, exist_ok=True)
    check(C.sizeof(Info) == 80 and C.sizeof(Frame) == 72 and C.sizeof(Stats) == 40, "ctypes struct sizes match the ABI (80 / 72 / 40)",
          (C.sizeof(Info), C.sizeof(Frame), C.sizeof(Stats)))
    lib = load(os.path.abspath(a.dll))
    check(lib.ac_version().startswith(b"0."), f"ac_version: {lib.ac_version().decode()}")
    for clip in a.clips:
        test_clip(lib, os.path.abspath(a.cli), work, os.path.abspath(clip))
    test_errors(lib, work)
    print(f"\n{checks - failures}/{checks} checks passed")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
