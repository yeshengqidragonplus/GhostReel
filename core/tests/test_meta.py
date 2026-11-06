"""Sidecar metadata (clip.acmeta.json, schema 1) through the shared library, with ctypes. M4.

Run isolated:
  python -I core/tests/test_meta.py --dll build/core-win64/bin/alphaclip.dll alpha_clip.webm opaque_clip.webm

The first clip must carry an alpha stream, the second must not; both must be at least 4 pixels wide and 2 high and
have an even width (the layout checks below derive their expectations from the actual video size).
What is tested: auto mode without a sidecar, every field of a valid sidecar, how the alpha stream is treated per
mode, the packed layouts and their rejections, every kind of invalid or unsupported sidecar (with the status code the
header documents), and a deterministic mutation fuzz of the sidecar text (no crash, always a documented status).
"""
import argparse
import ctypes as C
import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_shared_lib import (AC_AGAIN, AC_END, AC_ERR_ARG, AC_ERR_FORMAT, AC_ERR_OOM, AC_ERR_UNSUPPORTED,  # noqa: E402
                             AC_OK, Frame, Info, load)
import test_shared_lib as base  # noqa: E402

ALPHA_OPAQUE, PACKED_RIGHT, PACKED_BOTTOM, ALPHA_NATIVE = 0, 1, 2, 3
RANGE_UNSPEC, RANGE_LIMITED, RANGE_FULL = 0, 1, 2
MATRIX_UNSPEC, MATRIX_601, MATRIX_709 = 0, 1, 2

check = base.check


def open_meta(lib, data, meta):
    """Returns (status, info_or_None, last_error_text). Closes the player again."""
    buf = C.create_string_buffer(data, len(data))
    p = C.c_void_p(12345)  # a stale value: the library must clear it on failure
    mj = None if meta is None else (meta if isinstance(meta, bytes) else meta.encode())
    st = lib.ac_open_memory(buf, len(data), mj, C.byref(p))
    if st != AC_OK:
        err = (lib.ac_last_error(None) or b"").decode("utf-8", "replace")
        return st, (None if p.value is None else "out not cleared"), err
    info = Info()
    assert lib.ac_get_info(p, C.byref(info)) == AC_OK
    lib.ac_close(p)
    return st, info, ""


def expect(lib, data, meta, status, label, text=None):
    st, info, err = open_meta(lib, data, meta)
    ok = st == status and (info is None or status == AC_OK)
    if status != AC_OK:
        ok = ok and info is None and len(err) > 0 and (text is None or text in err)
    check(ok, label, f"status {st} (want {status}), error '{err}'" + ("" if info is None or status == AC_OK else ", out not cleared"))
    return st, info, err


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", required=True)
    ap.add_argument("alpha_clip")
    ap.add_argument("opaque_clip")
    ap.add_argument("--mutations", type=int, default=3000, help="mutated sidecars in the fuzz section (default 3000)")
    ap.add_argument("--seed", type=int, default=20261106)
    args = ap.parse_args()
    lib = load(os.path.abspath(args.dll))
    alpha = open(args.alpha_clip, "rb").read()
    opaque = open(args.opaque_clip, "rb").read()
    print("core", lib.ac_version().decode())

    # video sizes (auto mode)
    _, ia, _ = open_meta(lib, alpha, None)
    _, io, _ = open_meta(lib, opaque, None)
    W, H = io.width, io.height
    check(ia.alpha_mode == ALPHA_NATIVE and ia.has_alpha_plane == 1 and (ia.content_w, ia.content_h) == (ia.width, ia.height),
          "auto mode, alpha file: native_alpha, A plane, content == video")
    check(io.alpha_mode == ALPHA_OPAQUE and io.has_alpha_plane == 0 and (io.content_w, io.content_h) == (W, H),
          "auto mode, plain file: opaque, no A plane, content == video")
    check((ia.premultiplied, ia.color_range, ia.matrix, ia.loop_hint, ia.alpha_x, ia.alpha_y, ia.reserved) == (0,) * 7,
          "auto mode: premultiplied 0, range / matrix unspecified, no loop hint, alpha origin 0,0, reserved 0")
    st, i2, _ = open_meta(lib, opaque, "")
    check(st == AC_OK and i2.alpha_mode == ALPHA_OPAQUE, "an empty meta_json string means no sidecar")
    expect(lib, opaque, " ", AC_ERR_FORMAT, "a whitespace-only meta_json is not 'no sidecar': malformed", "sidecar metadata")

    # --- a valid sidecar, every field
    st, i, _ = open_meta(lib, alpha, '{"schema":1,"alpha_mode":"native_alpha","premultiplied":true,"color_range":"full",'
                                      '"matrix":"bt709","loop_hint":true,"content_size":[%d,%d]}' % (ia.width, ia.height))
    check(st == AC_OK and i.alpha_mode == ALPHA_NATIVE and i.premultiplied == 1 and i.color_range == RANGE_FULL and
          i.matrix == MATRIX_709 and i.loop_hint == 1 and i.has_alpha_plane == 1, "native_alpha sidecar: every field arrives in ac_info")
    st, i, _ = open_meta(lib, alpha, '{"schema":1,"alpha_mode":"native_alpha","color_range":"limited","matrix":"bt601"}')
    check(st == AC_OK and i.color_range == RANGE_LIMITED and i.matrix == MATRIX_601 and i.premultiplied == 0 and i.loop_hint == 0,
          "limited / bt601, defaults for the rest")
    st, i, _ = open_meta(lib, opaque, b"\xef\xbb\xbf" + b'{"schema":1,"alpha_mode":"opaque"}')
    check(st == AC_OK, "a UTF-8 byte-order mark in front of the JSON is accepted (Windows tools write one)")
    st, i, _ = open_meta(lib, opaque, '{"schema":1,"alpha_mode":"opaque","future_field":{"a":[1,2,{"b":null}]},"keyed":{"lut":"x.png"},"edge":{"shrink":0,"feather":0.0,"denoise":0e5}}')
    check(st == AC_OK, "unknown keys are ignored; keyed section is ignored outside keyed mode; all-zero edge is accepted")
    st, i, _ = open_meta(lib, opaque, '\n\t {"schema" : 1 ,\r\n "alpha_mode" : "opaque"} \n')
    check(st == AC_OK, "whitespace between tokens is accepted")
    st, i, _ = open_meta(lib, opaque, '{"schema":1,"alpha_mode":"\\u006fpaque"}')
    check(st == AC_OK and i.alpha_mode == ALPHA_OPAQUE, "a \\u escape inside a value is decoded")

    # --- how the alpha stream is treated
    st, i, _ = open_meta(lib, alpha, '{"schema":1,"alpha_mode":"opaque"}')
    check(st == AC_OK and i.has_alpha_plane == 0 and i.alpha_mode == ALPHA_OPAQUE, "opaque sidecar on an alpha file: the alpha stream is ignored")
    buf = C.create_string_buffer(alpha, len(alpha))
    p = C.c_void_p()
    assert lib.ac_open_memory(buf, len(alpha), b'{"schema":1,"alpha_mode":"opaque"}', C.byref(p)) == AC_OK
    f = Frame()
    n = 0
    while lib.ac_decode_next(p, C.byref(f)) == AC_OK:
        n += 1
        if f.plane[3]:
            n = -10 ** 6
            break
    check(n == ia.packet_count, f"opaque sidecar on an alpha file: {n} frames, none with a 4th plane")
    lib.ac_close(p)
    p = C.c_void_p()
    assert lib.ac_open_memory(buf, len(alpha), b'{"schema":1,"alpha_mode":"opaque"}', C.byref(p)) == AC_OK
    assert lib.ac_set_queue_depth(p, 4) == AC_OK and lib.ac_start(p) == AC_OK
    got, planes3 = 0, 0
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 8:
        clock = int((time.perf_counter() - t0) * 1e6 * 2)  # the clip at double speed
        s = lib.ac_acquire_frame(p, clock, C.byref(f))
        if s == AC_OK:
            got += 1
            planes3 += 1 if f.plane[3] else 0
            lib.ac_release_frame(p, C.byref(f))
        elif s == AC_END:
            break
        else:
            time.sleep(0.002)
    check(got > 0 and planes3 == 0, f"opaque sidecar on an alpha file, threaded player: {got} frames, none with a 4th plane")
    lib.ac_close(p)
    expect(lib, opaque, '{"schema":1,"alpha_mode":"native_alpha"}', AC_ERR_FORMAT, "native_alpha sidecar on a file without alpha is contradictory", "no alpha stream")

    # --- packed layouts on a plain file (the file's own size decides)
    st, i, _ = open_meta(lib, opaque, '{"schema":1,"alpha_mode":"packed_right"}')
    ok_right = W % 4 == 0
    check((st == AC_OK) == ok_right and (not ok_right or (i.content_w, i.content_h, i.alpha_x, i.alpha_y, i.has_alpha_plane) == (W // 2, H, W // 2, 0, 0)),
          f"packed_right without content_size on {W}x{H}: content {W // 2}x{H}, alpha image at x={W // 2}")
    st, i, _ = open_meta(lib, opaque, '{"schema":1,"alpha_mode":"packed_bottom"}')
    ok_bottom = H % 4 == 0
    check((st == AC_OK) == ok_bottom and (not ok_bottom or (i.content_w, i.content_h, i.alpha_x, i.alpha_y) == (W, H // 2, 0, H // 2)),
          f"packed_bottom without content_size on {W}x{H}: content {W}x{H // 2}, alpha image at y={H // 2}")
    cw, ch = (W // 2) // 2 * 2, 2 * ((H - 2) // 2 // 2)
    st, i, _ = open_meta(lib, opaque, '{"schema":1,"alpha_mode":"packed_right","content_size":[%d,%d],"premultiplied":true}' % (cw, ch))
    check(st == AC_OK and (i.content_w, i.content_h, i.alpha_x, i.alpha_y) == (cw, ch, cw, 0) and i.premultiplied == 1 and i.width == W,
          f"packed_right with a smaller content_size {cw}x{ch}: the alpha image starts at x={cw}, coded size stays {W}x{H}")
    st, i, _ = open_meta(lib, opaque, '{"schema":1,"alpha_mode":"packed_bottom","content_size":[%d,%d]}' % (W - W % 2, H // 2 - (H // 2) % 2))
    check(st == AC_OK and i.alpha_x == 0 and i.alpha_y == i.content_h, "packed_bottom with an explicit content_size: the alpha image starts at y = content height")
    expect(lib, opaque, '{"schema":1,"alpha_mode":"packed_right","content_size":[%d,%d]}' % (W // 2 + 2 - (W // 2) % 2, H),
           AC_ERR_FORMAT, "packed_right: picture plus alpha image wider than the video is rejected", "needs")
    expect(lib, opaque, '{"schema":1,"alpha_mode":"packed_bottom","content_size":[%d,%d]}' % (W, H // 2 + 2 - (H // 2) % 2),
           AC_ERR_FORMAT, "packed_bottom: picture plus alpha image taller than the video is rejected", "needs")
    expect(lib, opaque, '{"schema":1,"alpha_mode":"packed_right","content_size":[%d,%d]}' % (cw - 1, ch),
           AC_ERR_FORMAT, "packed_right: odd picture width is rejected (chroma would straddle the border)", "even")
    expect(lib, opaque, '{"schema":1,"alpha_mode":"packed_right","content_size":[%d,%d]}' % (cw, ch - 1),
           AC_ERR_FORMAT, "packed_right: odd picture height is rejected", "even")
    expect(lib, opaque, '{"schema":1,"alpha_mode":"packed_right","content_size":[0,%d]}' % ch, AC_ERR_FORMAT, "content_size 0 is rejected")
    expect(lib, alpha, '{"schema":1,"alpha_mode":"packed_right"}', AC_ERR_FORMAT, "packed_right on a file that also carries an alpha stream is contradictory", "alpha stream")
    expect(lib, opaque, '{"schema":1,"alpha_mode":"opaque","content_size":[%d,%d]}' % (W // 2 or 1, H), AC_ERR_FORMAT,
           "opaque with a content_size that is not the video size is rejected", "differs")
    st, i, _ = open_meta(lib, opaque, '{"schema":1,"alpha_mode":"opaque","content_size":[%d,%d]}' % (W, H))
    check(st == AC_OK, "opaque with content_size equal to the video size is accepted")

    # --- invalid / unsupported sidecars: the status code is part of the contract
    bad = [
        ('[]', AC_ERR_FORMAT, "top level is an array"),
        ('"x"', AC_ERR_FORMAT, "top level is a string"),
        ('null', AC_ERR_FORMAT, "top level is null"),
        ('{}', AC_ERR_FORMAT, "no schema"),
        ('{"alpha_mode":"opaque"}', AC_ERR_FORMAT, "schema missing"),
        ('{"schema":"1","alpha_mode":"opaque"}', AC_ERR_FORMAT, "schema is a string"),
        ('{"schema":1.0,"alpha_mode":"opaque"}', AC_ERR_FORMAT, "schema 1.0 is not an integer"),
        ('{"schema":1e0,"alpha_mode":"opaque"}', AC_ERR_FORMAT, "schema 1e0 is not an integer"),
        ('{"schema":true,"alpha_mode":"opaque"}', AC_ERR_FORMAT, "schema is a boolean"),
        ('{"schema":2,"alpha_mode":"opaque"}', AC_ERR_UNSUPPORTED, "schema 2 is unsupported"),
        ('{"schema":0,"alpha_mode":"opaque"}', AC_ERR_UNSUPPORTED, "schema 0 is unsupported"),
        ('{"schema":-1,"alpha_mode":"opaque"}', AC_ERR_UNSUPPORTED, "schema -1 is unsupported"),
        ('{"schema":99999999999999999999,"alpha_mode":"opaque"}', AC_ERR_FORMAT, "schema beyond int64 is not an integer"),
        ('{"schema":1}', AC_ERR_FORMAT, "alpha_mode missing"),
        ('{"schema":1,"alpha_mode":3}', AC_ERR_FORMAT, "alpha_mode is a number"),
        ('{"schema":1,"alpha_mode":"Opaque"}', AC_ERR_FORMAT, "alpha_mode is case sensitive"),
        ('{"schema":1,"alpha_mode":"sideways"}', AC_ERR_FORMAT, "unknown alpha_mode"),
        ('{"schema":1,"alpha_mode":"keyed"}', AC_ERR_UNSUPPORTED, "keyed is not implemented"),
        ('{"schema":1,"alpha_mode":"opaque","premultiplied":1}', AC_ERR_FORMAT, "premultiplied must be a boolean"),
        ('{"schema":1,"alpha_mode":"opaque","loop_hint":"yes"}', AC_ERR_FORMAT, "loop_hint must be a boolean"),
        ('{"schema":1,"alpha_mode":"opaque","matrix":"bt2020"}', AC_ERR_FORMAT, "unknown matrix"),
        ('{"schema":1,"alpha_mode":"opaque","matrix":709}', AC_ERR_FORMAT, "matrix must be a string"),
        ('{"schema":1,"alpha_mode":"opaque","color_range":"tv"}', AC_ERR_FORMAT, "unknown color_range"),
        ('{"schema":1,"alpha_mode":"opaque","content_size":[640]}', AC_ERR_FORMAT, "content_size with one element"),
        ('{"schema":1,"alpha_mode":"opaque","content_size":[1,2,3]}', AC_ERR_FORMAT, "content_size with three elements"),
        ('{"schema":1,"alpha_mode":"opaque","content_size":"640x360"}', AC_ERR_FORMAT, "content_size is a string"),
        ('{"schema":1,"alpha_mode":"opaque","content_size":[640.5,360]}', AC_ERR_FORMAT, "content_size with a fraction"),
        ('{"schema":1,"alpha_mode":"opaque","content_size":[-4,360]}', AC_ERR_FORMAT, "content_size negative"),
        ('{"schema":1,"alpha_mode":"opaque","content_size":[100000,360]}', AC_ERR_FORMAT, "content_size above the maximum dimension"),
        ('{"schema":1,"alpha_mode":"opaque","content_size":["a","b"]}', AC_ERR_FORMAT, "content_size of strings"),
        ('{"schema":1,"alpha_mode":"opaque","edge":3}', AC_ERR_FORMAT, "edge is not an object"),
        ('{"schema":1,"alpha_mode":"opaque","edge":{"shrink":-1}}', AC_ERR_FORMAT, "edge.shrink negative"),
        ('{"schema":1,"alpha_mode":"opaque","edge":{"feather":"a"}}', AC_ERR_FORMAT, "edge.feather not a number"),
        ('{"schema":1,"alpha_mode":"opaque","edge":{"shrink":0.5}}', AC_ERR_UNSUPPORTED, "edge.shrink non-zero is not implemented"),
        ('{"schema":1,"alpha_mode":"opaque","edge":{"denoise":1e-3}}', AC_ERR_UNSUPPORTED, "edge.denoise non-zero is not implemented"),
        ('{"schema":1,"schema":1,"alpha_mode":"opaque"}', AC_ERR_FORMAT, "duplicate key"),
        ('{"schema":1,"alpha_mode":"opaque",}', AC_ERR_FORMAT, "trailing comma in an object"),
        ('{"schema":1,"alpha_mode":"opaque","a":[1,]}', AC_ERR_FORMAT, "trailing comma in an array"),
        ('{"schema":1,"alpha_mode":"opaque"} x', AC_ERR_FORMAT, "content after the value"),
        ('{"schema":1,"alpha_mode":"opaque"}{}', AC_ERR_FORMAT, "two values"),
        ("{'schema':1,'alpha_mode':'opaque'}", AC_ERR_FORMAT, "single quotes"),
        ('{schema:1}', AC_ERR_FORMAT, "unquoted keys"),
        ('{"schema":01,"alpha_mode":"opaque"}', AC_ERR_FORMAT, "leading zero"),
        ('{"schema":+1,"alpha_mode":"opaque"}', AC_ERR_FORMAT, "plus sign"),
        ('{"schema":1.,"alpha_mode":"opaque"}', AC_ERR_FORMAT, "digit missing after the point"),
        ('{"schema":1e,"alpha_mode":"opaque"}', AC_ERR_FORMAT, "digit missing in the exponent"),
        ('{"schema":-,"alpha_mode":"opaque"}', AC_ERR_FORMAT, "lone minus"),
        ('{"schema":1,"alpha_mode":"opaque","a":1e999}', AC_ERR_FORMAT, "number out of range"),
        ('{"schema":1,"alpha_mode":"opaque","a":tru}', AC_ERR_FORMAT, "broken literal"),
        ('{"schema":1,"alpha_mode":"opaque","a":nul}', AC_ERR_FORMAT, "broken null"),
        ('{"schema":1,"alpha_mode":"opaque', AC_ERR_FORMAT, "unterminated string"),
        ('{"schema":1,"alpha_mode":"opaque"', AC_ERR_FORMAT, "unterminated object"),
        ('{"schema":1,"alpha_mode":"opaque","a":[1', AC_ERR_FORMAT, "unterminated array"),
        ('{"schema":1,"alpha_mode":"op\\aque"}', AC_ERR_FORMAT, "invalid escape"),
        ('{"schema":1,"alpha_mode":"op\\u00zzque"}', AC_ERR_FORMAT, "invalid \\u escape"),
        ('{"schema":1,"alpha_mode":"op\\ud800que"}', AC_ERR_FORMAT, "lone high surrogate"),
        ('{"schema":1,"alpha_mode":"op\\udc00que"}', AC_ERR_FORMAT, "lone low surrogate"),
        ('{"schema":1,"alpha_mode":"op\\u0000que"}', AC_ERR_FORMAT, "NUL escape"),
        ('{"schema":1,"alpha_mode":"op\tque"}', AC_ERR_FORMAT, "raw control character in a string"),
        ('{"schema":1,"alpha_mode":"opaque","a":' + "[" * 20 + "]" * 20 + '}', AC_ERR_FORMAT, "nesting beyond the depth limit"),
        ('{"schema":1,"alpha_mode":"opaque","a":"' + "x" * 5000 + '"}', AC_ERR_FORMAT, "string longer than the limit"),
        ('{"schema":1,"alpha_mode":"opaque","a":[' + ",".join("0" * 1 for _ in range(70)) + ']}', AC_ERR_FORMAT, "too many array elements"),
        ('{"schema":1,"alpha_mode":"opaque",' + ",".join('"k%d":0' % n for n in range(70)) + '}', AC_ERR_FORMAT, "too many object members"),
    ]
    for text, status, label in bad:
        expect(lib, opaque, text, status, f"invalid sidecar: {label}", "sidecar metadata")
    for raw, label in [(b'{"schema":1,"alpha_mode":"opa\xffque"}', "invalid UTF-8 byte"),
                       (b'{"schema":1,"alpha_mode":"op\xc0\x80que"}', "overlong UTF-8"),
                       (b'{"schema":1,"alpha_mode":"op\xed\xa0\x80que"}', "UTF-8 encoded surrogate"),
                       (b'{"schema":1,"alpha_mode":"op\xf4\x90\x80\x80que"}', "UTF-8 above U+10FFFF"),
                       (b'{"schema":1,"alpha_mode":"op\xe2\x82"}', "truncated UTF-8 sequence")]:
        expect(lib, opaque, raw, AC_ERR_FORMAT, f"invalid sidecar: {label}", "sidecar metadata")
    st, i, _ = open_meta(lib, opaque, '{"schema":1,"alpha_mode":"opaque","note":"café 中文 \U0001f600 ok"}'.encode("utf-8"))
    check(st == AC_OK, "valid multi-byte UTF-8 inside an ignored value is accepted")
    st, i, _ = open_meta(lib, opaque, '{"schema":1,"alpha_mode":"opaque","note":"\\ud83d\\ude00"}')
    check(st == AC_OK, "a surrogate pair written as two \\u escapes is accepted")
    big = '{"schema":1,"alpha_mode":"opaque","pad":"' + "x" * 70000 + '"}'
    expect(lib, opaque, big, AC_ERR_OOM, "a sidecar over 64 KiB is refused", "larger than")

    # --- the sidecar is checked before the file; a bad file with a good sidecar still fails as a file
    junk = b"this is not a webm file at all" * 4
    st, info, err = open_meta(lib, junk, '{"schema":1,"alpha_mode":"opaque"}')
    check(st == AC_ERR_FORMAT and "sidecar" not in err, "good sidecar + garbage file: the container error is reported", err)
    st, info, err = open_meta(lib, junk, '{"schema":1')
    check(st == AC_ERR_FORMAT and "sidecar" in err, "bad sidecar + garbage file: the sidecar error is reported first", err)
    p = C.c_void_p(7)
    check(lib.ac_open_file(os.path.join(os.path.dirname(args.opaque_clip), "does_not_exist.webm").encode(), b'{"schema":2,"alpha_mode":"opaque"}', C.byref(p)) == AC_ERR_UNSUPPORTED and not p.value,
          "ac_open_file checks the sidecar before it touches the file")
    st = lib.ac_open_file(args.opaque_clip.encode(), b'{"schema":1,"alpha_mode":"packed_bottom","content_size":[%d,%d]}' % (W - W % 2, H // 2 - (H // 2) % 2), C.byref(p))
    info = Info()
    check(st == AC_OK and lib.ac_get_info(p, C.byref(info)) == AC_OK and info.alpha_mode == PACKED_BOTTOM, "ac_open_file with a sidecar works like ac_open_memory")
    lib.ac_close(p)

    # --- deterministic mutation fuzz of the sidecar text: no crash, always a documented status, never a half-open player
    seeds = [b'{"schema":1,"alpha_mode":"native_alpha","premultiplied":true,"color_range":"full","matrix":"bt709","loop_hint":true}',
             b'{"schema":1,"alpha_mode":"packed_right","content_size":[%d,%d],"edge":{"shrink":0,"feather":0.0},"x":[1,2,{"y":"\\u00e9"}]}' % (cw, ch),
             b'{"schema":1,"alpha_mode":"opaque"}']
    rng = random.Random(args.seed)
    bad_status = 0
    accepted = 0
    for n in range(args.mutations):
        text = bytearray(rng.choice(seeds))
        for _ in range(rng.randint(1, 4)):
            op = rng.randint(0, 4)
            pos = rng.randrange(len(text) + 1)
            if op == 0 and text:
                text[min(pos, len(text) - 1)] = rng.randrange(1, 256)
            elif op == 1 and text:
                del text[min(pos, len(text) - 1)]
            elif op == 2:
                text.insert(pos, rng.choice(b'{}[]",:\\0123456789-+.eEtfnu '))
            elif op == 3 and text:
                text[min(pos, len(text) - 1):] = b""
            else:
                text[pos:pos] = bytes(rng.randrange(1, 256) for _ in range(rng.randint(1, 8)))
        data = bytes(text).split(b"\x00")[0]  # a C string: an embedded NUL ends it
        buf2 = C.create_string_buffer(opaque, len(opaque))
        p = C.c_void_p(99)
        st = lib.ac_open_memory(buf2, len(opaque), data, C.byref(p))
        if st == AC_OK:
            accepted += 1
            inf = Info()
            sane = (lib.ac_get_info(p, C.byref(inf)) == AC_OK and 0 < inf.content_w <= inf.width and 0 < inf.content_h <= inf.height and
                    inf.alpha_mode in (0, 1, 2, 3) and inf.has_alpha_plane == 0 and inf.reserved == 0)
            lib.ac_close(p)
            if not sane:
                bad_status += 1
        elif st not in (AC_ERR_FORMAT, AC_ERR_UNSUPPORTED, AC_ERR_OOM) or p.value:
            bad_status += 1
    check(bad_status == 0, f"{args.mutations} mutated sidecars: no crash, every result a documented status or a sane info ({accepted} accepted)", bad_status)

    print(f"\n{base.checks - base.failures}/{base.checks} checks passed")
    return 1 if base.failures else 0


if __name__ == "__main__":
    sys.exit(main())
