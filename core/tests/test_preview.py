"""The preview (`alphaclip preview` + the page in tools/preview/www + the master shader in shaders/). M6.

Run isolated (needs numpy; for parts B and C a Chromium browser such as Chrome or Edge; nothing is downloaded):
  python -I core/tests/test_preview.py --cli build/core-win64/bin/alphaclip.exe --workdir <empty dir> \
      --truth samples/m4/truth_rgba_3f.raw [--browser "C:/Program Files/Google/Chrome/Application/chrome.exe"] \
      [--browser-arg=--use-angle=swiftshader ...] [--unity-gamma <dump dir>] [--unity-linear <dump dir>] [--only ABCD]

What is tested
  A  the local service, without a browser: the page's files are the ones in the repository; every frame it hands
     out is byte for byte what `alphaclip decode` writes (walking forward, seeking, asking out of order); the sidecar
     can be replaced and a refused one leaves the clip as it was; a file that is not WebM; more clips than it keeps;
     the command line; and the HTTP side: other hosts and origins are refused, malformed and hostile requests get an
     error or are dropped, an idle connection does not hold up the others, 300 mutated requests leave it running.
  B  the page's drawing code in a real browser (headless). autotest.html runs renderer.js on a list of jobs and
     posts the pixels back:
       - the shader's output against a CPU implementation of the same formulas (every alpha mode, Gamma and Linear,
         both matrices, both ranges, both chroma positions, odd sizes);
       - the composite against the CPU blend of that output;
       - the composite of the four M4 clips against the EXACT ground truth (the M4 criteria: mean |error| and the
         signed error on the soft edge), with the two controls that must fail (wrong premultiplied flag,
         straight-alpha blending);
       - magnification.
  C  the same pixels against what Unity drew (dumps written by the Unity edit-mode tests, run_editmode_tests.ps1
     -DumpDir): the shader output, the composites and the controls, Gamma and Linear. Side-by-side PNG files go to
     <workdir>/compare.
  D  the page itself (uitest.html drives index.html in a frame): playing shows every frame in order at the clip's
     speed, pause / step / seek / loop / the end of the clip, backgrounds, zoom, pan, the pixel read-out, the sidecar
     editor (a refused sidecar shows the core's reason and keeps the picture), files dropped on the page; and what
     the page has on screen equals the CPU reference.
"""
import argparse
import http.server
import json
import os
import random
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
failures = checks = 0
CLI = WORK = None

M4 = ["native_straight", "native_premul", "packed_right_premul", "packed_bottom_straight"]
TRUTH_W, TRUTH_H, TRUTH_FRAMES = 320, 180, (0, 20, 40)
BACKGROUNDS = [(255, 255, 255), (128, 128, 128), (0, 200, 230)]
# The M4 limits (bindings/unity/TestProject/Assets/Scripts/AlphaClipTruth.cs): the same numbers judge the preview.
MAX_MEAN_ABS = 1.0
MAX_EDGE_MEAN = 1.0
MIN_CONTROL = {"gamma": 10.0, "linear": 3.0}
# Preview against Unity: "the same to the eye" is taken as at most 2 levels on any pixel and 0.1 on average.
UNITY_MAX = 2
UNITY_MEAN = 0.1


def check(ok, label, detail=""):
    global failures, checks
    checks += 1
    if not ok:
        failures += 1
    print(("PASS  " if ok else "FAIL  ") + label + (("  -- " + str(detail)) if (detail and not ok) else ""), flush=True)


def sample(*parts):
    return os.path.join(ROOT, "samples", *parts)


def work(*parts):
    p = os.path.join(WORK, *parts)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    return p


def run(args):
    r = subprocess.run([str(a) for a in args], capture_output=True)
    return r.returncode, r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace")


def read(path, mode="rb"):
    with open(path, mode) as f:
        return f.read()


def sidecar_of(clip):
    p = os.path.splitext(clip)[0] + ".acmeta.json"
    return p if os.path.exists(p) else None


# ---- the service ---------------------------------------------------------------------------------------------------

OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))  # never through a system proxy


class Service:
    """One `alphaclip preview` process."""

    def __init__(self, *args):
        self.proc = subprocess.Popen([CLI, "preview", *args, "--no-open", "--port", "0"],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        line = self.proc.stdout.readline().decode("utf-8", "replace").strip()
        if not line.startswith("url: http://127.0.0.1:"):
            raise RuntimeError("alphaclip preview did not start: " + line + self.proc.stderr.read().decode("utf-8", "replace"))
        self.url = line[5:].rstrip("/")
        self.port = int(self.url.rsplit(":", 1)[1])

    def request(self, path, method="GET", data=None, headers=None):
        req = urllib.request.Request(self.url + path, method=method, data=data, headers=headers or {})
        try:
            with OPENER.open(req, timeout=60) as r:
                return r.status, dict(r.headers), r.read()
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers), e.read()

    def json(self, path, method="GET", data=None):
        status, _, body = self.request(path, method, data)
        return status, json.loads(body)

    def upload(self, name, data):
        status, body = self.json("/api/clips?name=" + urllib.parse.quote(name), "POST", data)
        assert status == 200, body
        return body["id"]

    def open(self, clip_id, meta):
        return self.json(f"/api/clips/{clip_id}/open", "POST", (meta or "").encode("utf-8"))

    def frame(self, clip_id, **query):
        status, _, body = self.request(f"/api/clips/{clip_id}/frame?" + urllib.parse.urlencode(query))
        return status, body

    def raw(self, payload, timeout=5.0, half_close=True):
        """Sends bytes as they are and returns whatever comes back until the server closes."""
        with socket.create_connection(("127.0.0.1", self.port), timeout=timeout) as s:
            try:
                s.sendall(payload)
                if half_close:
                    s.shutdown(socket.SHUT_WR)
                out = b""
                while True:
                    part = s.recv(65536)
                    if not part:
                        return out
                    out += part
            except OSError:
                return b""

    def alive(self):
        try:
            return self.request("/api/state")[0] == 200
        except OSError:
            return False

    def quit(self):
        try:
            self.request("/api/quit", "POST", b"")
        except OSError:
            pass
        try:
            return self.proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return None


class Frame:
    def __init__(self, body):
        assert body[:4] == b"ACF1", body[:16]
        self.width, self.height, flags = struct.unpack_from("<III", body, 4)
        self.pts = struct.unpack_from("<q", body, 16)[0]
        self.payload = body[32:]
        w, h = self.width, self.height
        cw, ch = (w + 1) // 2, (h + 1) // 2
        self.has_alpha = bool(flags & 1)
        assert len(self.payload) == w * h + 2 * cw * ch + (w * h if self.has_alpha else 0)
        d = np.frombuffer(self.payload, dtype=np.uint8)
        self.y = d[:w * h].reshape(h, w)
        self.u = d[w * h:w * h + cw * ch].reshape(ch, cw)
        self.v = d[w * h + cw * ch:w * h + 2 * cw * ch].reshape(ch, cw)
        self.a = d[w * h + 2 * cw * ch:].reshape(h, w) if self.has_alpha else None


def walk(srv, clip_id):
    """Every frame of a clip in order: the first one by time, the rest by 'the frame after'."""
    frames = []
    status, body = srv.frame(clip_id, at_us=0)
    while status == 200:
        frames.append(Frame(body))
        status, body = srv.frame(clip_id, after_us=frames[-1].pts)
    return frames, status


def part_a():
    print("== A: the local service")
    first = sample("m4", "native_premul.webm")
    srv = Service(first)
    try:
        # A1 the clip from the command line, with the sidecar next to it
        status, state = srv.json("/api/state")
        clip = state["clip"]
        check(status == 200 and clip and clip["id"] == 1 and clip["name"] == "native_premul.webm" and clip["error"] is None
              and clip["size"] == os.path.getsize(first), "the clip given on the command line is open", state)
        info = clip["info"]
        check(info["alpha_mode"] == 3 and info["premultiplied"] == 1 and info["width"] == 320 and info["content_h"] == 180
              and info["matrix"] == 1 and info["color_range"] == 1 and info["has_alpha_plane"] == 1 and info["packet_count"] == 60,
              "its info is what the core reads from the sidecar", info)
        check(clip["meta"] == read(sidecar_of(first), "r"), "the sidecar next to the clip was used")
        rc, out, _ = run([CLI, "inspect", first])
        skip = ("file: ", "sidecar: ")
        check(rc == 0 and [l for l in out.splitlines() if not l.startswith(skip)] ==
              [l for l in clip["inspect"].splitlines() if not l.startswith(skip)],
              "the page gets the same report as `alphaclip inspect`")

        # A2 the page's files are the repository's
        cmake = read(os.path.join(ROOT, "tools", "cli", "CMakeLists.txt"), "r")
        assets = re.findall(r'"(/[^|"]+)\|([^|"]+)\|([^|"]+)"', cmake)
        check(len(assets) >= 8, "the list of embedded files was found", len(assets))
        same = True
        for url, source, ctype in assets:
            status, headers, body = srv.request(url)
            same = same and status == 200 and body == read(os.path.join(ROOT, source)) and headers["Content-Type"].startswith(ctype)
        check(same, "every embedded file is byte for byte the one in the repository, with its content type")
        status, headers, body = srv.request("/")
        check(status == 200 and body == read(os.path.join(ROOT, "tools", "preview", "www", "index.html")) and
              "default-src 'self'" in headers.get("Content-Security-Policy", "") and headers.get("Cache-Control") == "no-store",
              "/ is the page, with a content security policy and no caching")
        codes = [srv.request(p)[0] for p in ("/nope", "/app.js/", "/tools/cli/main.cpp", "/..%2f..%2fCLAUDE.md", "/api/nope")]
        check(codes == [404] * 5, "addresses that are not in the list do not exist", codes)
        check(srv.request("/", "POST", b"x")[0] == 405 and srv.request("/api/state", "POST", b"")[0] == 405 and
              srv.request("/api/quit")[0] == 405 and srv.request("/api/clips")[0] == 405, "wrong methods are refused")

        # A3 frames are exactly what `alphaclip decode` writes
        clips = [sample("m4", n + ".webm") for n in M4] + [sample("m0", "alpha_native.webm"), sample("m0", "opaque.webm")] + \
                [sample("m1", n + ".webm") for n in ("opaque_altref", "opaque_odd", "alpha_odd", "alpha_scenecut")]
        rng = random.Random(6)
        for path in clips:
            name = os.path.basename(path)
            meta_path = sidecar_of(path)
            raw_path = work("a", name + ".raw")
            rc, out, err = run([CLI, "decode", path, "--out", raw_path] + (["--meta", meta_path] if meta_path else []))
            assert rc == 0, err
            reference = read(raw_path)
            cid = srv.upload(name, read(path))
            status, opened = srv.open(cid, read(meta_path, "r") if meta_path else None)
            assert status == 200, opened
            frames, end = walk(srv, cid)
            check(end == 204 and b"".join(f.payload for f in frames) == reference and len(frames) > 0,
                  f"{name}: walking forward gives every frame of `alphaclip decode` ({len(frames)}), then 204")
            pts = [f.pts for f in frames]
            check(all(b > a for a, b in zip(pts, pts[1:])), f"{name}: the timestamps increase")
            ok = True
            for _ in range(8):   # seeks
                t = rng.randrange(-5000, pts[-1] + 60000)
                want = max([i for i, p in enumerate(pts) if p <= t], default=0)
                status, body = srv.frame(cid, at_us=t)
                ok = ok and status == 200 and Frame(body).pts == pts[want] and Frame(body).payload == frames[want].payload
            for _ in range(6):   # "the frame after" from anywhere, not only from where the decoder stands
                i = rng.randrange(0, len(pts))
                status, body = srv.frame(cid, after_us=pts[i])
                if i + 1 < len(pts):
                    ok = ok and status == 200 and Frame(body).payload == frames[i + 1].payload
                else:
                    ok = ok and status == 204
            status, body = srv.frame(cid, after_us=-1)
            ok = ok and status == 200 and Frame(body).pts == pts[0]
            status, body = srv.frame(cid, at_us=pts[len(pts) // 2])
            again = srv.frame(cid, at_us=pts[len(pts) // 2])
            ok = ok and status == 200 and again == (status, body)
            check(ok, f"{name}: seeking, asking out of order, before the start and twice for the same frame")
        last_id = cid

        # A4 the sidecar can be replaced; a refused one leaves the clip as it was
        cid = srv.upload("native_premul.webm", read(first))
        good = read(sidecar_of(first), "r")
        status, opened = srv.open(cid, good)
        check(status == 200 and opened["info"]["alpha_mode"] == 3 and opened["meta"] == good, "a clip sent to the service opens with its sidecar")
        status, body = srv.frame(cid, at_us=0)
        with_alpha = Frame(body)
        status, opened = srv.open(cid, '{"schema":1,"alpha_mode":"opaque"}')
        status2, body = srv.frame(cid, at_us=0)
        check(status == 200 and opened["info"]["alpha_mode"] == 0 and opened["info"]["has_alpha_plane"] == 0 and status2 == 200 and
              not Frame(body).has_alpha and with_alpha.has_alpha and Frame(body).payload == with_alpha.payload[:len(Frame(body).payload)],
              "re-opened as opaque: the same colour planes, no alpha plane")
        status, opened = srv.open(cid, None)
        check(status == 200 and opened["meta"] is None and opened["info"]["alpha_mode"] == 3 and opened["info"]["matrix"] == 0,
              "re-opened without a sidecar: the core's auto mode")
        refusals = [('{"schema":1,', "AC_ERR_FORMAT"), ('{"schema":2,"alpha_mode":"opaque"}', "AC_ERR_UNSUPPORTED"),
                    ('{"schema":1,"alpha_mode":"keyed"}', "AC_ERR_UNSUPPORTED"), ('{"schema":1,"alpha_mode":"packed_right"}', "AC_ERR_FORMAT"),
                    ('{"schema":1,"alpha_mode":"native_alpha","edge":{"shrink":1}}', "AC_ERR_UNSUPPORTED"), ("   ", "AC_ERR_FORMAT"),
                    ('{"schema":1,"alpha_mode":"opaque","x":"' + "y" * 70000 + '"}', "AC_ERR_OOM")]
        ok = True
        detail = ""
        for text, want in refusals:
            status, answer = srv.open(cid, text)
            if not (status == 422 and answer["status"] == want and answer["error"]):
                ok, detail = False, (text[:40], status, answer)
        status, state = srv.json("/api/state")
        status2, body = srv.frame(cid, at_us=0)
        check(ok and state["clip"]["id"] == cid and state["clip"]["meta"] is None and state["clip"]["info"]["alpha_mode"] == 3 and
              status2 == 200 and Frame(body).payload == with_alpha.payload,
              f"{len(refusals)} sidecars the core refuses: 422 with the core's status, and the clip stays as it was", detail)
        check(srv.request(f"/api/clips/{cid}/open", "POST", b"x" * (1 << 20) + b"x")[0] == 413, "a sidecar of more than 1 MiB is not even tried")
        codes = [srv.frame(cid)[0], srv.frame(cid, at_us=0, after_us=0)[0], srv.frame(cid, at_us="abc")[0], srv.frame(cid, at_us="1e3")[0],
                 srv.frame(cid, at_us="99999999999999999999")[0], srv.request(f"/api/clips/{cid}/nope")[0],
                 srv.request("/api/clips/999/frame?at_us=0")[0], srv.request("/api/clips/abc/frame?at_us=0")[0]]
        check(codes == [400, 400, 400, 400, 400, 404, 404, 404], "bad frame requests are refused", codes)

        # A5 not a WebM file
        bad = srv.upload("notes.webm", b"this is not a video" * 100)
        status, answer = srv.open(bad, None)
        status2, state = srv.json("/api/state")
        check(status == 422 and answer["status"] == "AC_ERR_FORMAT" and state["clip"]["id"] == bad and state["clip"]["info"] is None and
              state["clip"]["error"] and srv.frame(bad, at_us=0)[0] == 409,
              "a file that is not WebM: refused with the reason, no frames", (status, answer))
        check(srv.request("/api/clips?name=x.webm", "POST", b"")[0] == 400, "an empty upload is refused")
        truncated = srv.upload("cut.webm", read(first)[:20000])
        status, answer = srv.open(truncated, None)
        if status == 200:
            frames, end = walk(srv, truncated)
            check(end in (204, 500) and len(frames) < 60, "a truncated clip: frames up to the damage, then an error or the end", (end, len(frames)))
        else:
            check(status == 422, "a truncated clip is refused", (status, answer))
        odd_name = srv.upload("目录/片子 \"1\"\\x.webm", read(first))
        status, opened = srv.open(odd_name, None)
        check(status == 200 and opened["name"].endswith("x.webm"), "a file name with a path, quotes and non-ASCII characters", opened.get("name"))

        # A6 only a few clips are kept
        check(srv.frame(last_id, at_us=0)[0] == 404 and srv.frame(odd_name, at_us=0)[0] == 200,
              "the least recently used clip made room: its address is gone, the newest one answers")
        check(srv.alive(), "the service is still running")

        part_a_http(srv)
    finally:
        rc = srv.quit()
    check(rc == 0, "POST /api/quit ends the process with exit code 0", rc)

    # A8 the command line
    check(run([CLI, "preview", work("a", "missing.webm"), "--no-open", "--port", "0"])[0] == 1, "a clip that does not exist: exit code 1")
    check(run([CLI, "preview", "--port", "70000"])[0] == 2 and run([CLI, "preview", "--meta", "x.json"])[0] == 2 and
          run([CLI, "preview", first, "--bogus"])[0] == 2 and run([CLI, "preview", first, first])[0] == 2, "bad arguments: usage, exit code 2")
    a = Service()
    try:
        status, state = a.json("/api/state")
        check(status == 200 and state["clip"] is None and state["version"], "without a clip the service starts empty")
        rc, out, err = run([CLI, "preview", "--no-open", "--port", str(a.port)])
        check(rc == 1 and "in use" in err, "a port that is taken: exit code 1 and a message", (rc, err))
    finally:
        a.quit()
    bad_meta = work("a", "bad.acmeta.json")
    with open(bad_meta, "w") as f:
        f.write('{"schema":1,"alpha_mode":"packed_right"}')
    b = Service(first, "--meta", bad_meta)
    try:
        status, state = b.json("/api/state")
        check(state["clip"]["info"] is None and "packed_right" in state["clip"]["error"] and state["clip"]["meta"] == read(bad_meta, "r"),
              "a clip whose sidecar the core refuses: the service starts and reports the reason", state["clip"]["error"])
        status, opened = b.open(1, None)
        check(status == 200 and opened["info"]["alpha_mode"] == 3, "... and the page can open it with another sidecar")
    finally:
        b.quit()
    c = Service(first, "--no-meta")
    try:
        status, state = c.json("/api/state")
        check(state["clip"]["meta"] is None and state["clip"]["info"]["matrix"] == 0 and "--no-meta" in state["clip"]["inspect"],
              "--no-meta ignores the sidecar next to the clip")
    finally:
        c.quit()


def part_a_http(srv):
    """The HTTP side: who may ask, and requests no browser would send."""
    host = f"Host: 127.0.0.1:{srv.port}\r\n".encode()

    def status_of(answer):
        return int(answer.split(b" ", 2)[1]) if answer.startswith(b"HTTP/1.1 ") else 0

    get = lambda extra=b"", path=b"/api/state": status_of(srv.raw(b"GET " + path + b" HTTP/1.1\r\n" + extra + b"\r\n"))
    check(get(host) == 200, "a plain request written by hand is answered")
    check(get(f"Host: localhost:{srv.port}\r\n".encode()) == 200, "localhost is this server too")
    codes = [get(b""), get(b"Host: evil.example\r\n"), get(f"Host: 127.0.0.1:{srv.port + 1}\r\n".encode()),
             get(f"Host: evil.example:{srv.port}\r\n".encode()), get(host + b"Host: evil.example\r\n")]
    check(codes == [403, 403, 403, 403, 400], "requests for another host (DNS rebinding) are refused", codes)
    codes = [get(host + b"Origin: http://evil.example\r\n"), get(host + b"Origin: null\r\n"),
             get(host + f"Origin: https://127.0.0.1:{srv.port}\r\n".encode()),
             get(host + f"Origin: http://127.0.0.1:{srv.port}\r\n".encode())]
    check(codes == [403, 403, 403, 200], "requests from another origin are refused, its own origin is fine", codes)
    codes = [get(host + b"Sec-Fetch-Site: cross-site\r\n"), get(host + b"Sec-Fetch-Site: same-site\r\n"),
             get(host + b"Sec-Fetch-Site: same-origin\r\n"), get(host + b"Sec-Fetch-Site: none\r\n"),
             get(host + b"Sec-Fetch-Site: cross-site\r\n", b"/")]
    check(codes == [403, 403, 200, 200, 200], "the API refuses requests a browser marks as coming from another site", codes)

    bad = [b"GET\r\n\r\n", b"GET /api/state\r\n" + host + b"\r\n", b"get /api/state HTTP/1.1\r\n" + host + b"\r\n",
           b"GET api/state HTTP/1.1\r\n" + host + b"\r\n", b"GET /api/state HTTP/2.0\r\n" + host + b"\r\n",
           b"GET /a b HTTP/1.1\r\n" + host + b"\r\n", b"GET /%zz HTTP/1.1\r\n" + host + b"\r\n", b"GET /%00 HTTP/1.1\r\n" + host + b"\r\n",
           b"GET /api/state?a=%f HTTP/1.1\r\n" + host + b"\r\n", b"GET /api/state HTTP/1.1\r\n" + host + b"NoColon\r\n\r\n",
           b"POST /api/quit HTTP/1.1\r\n" + host + b"Content-Length: 12abc\r\n\r\n", b"POST /api/quit HTTP/1.1\r\n" + host + b"Content-Length: -1\r\n\r\n",
           b"POST /api/quit HTTP/1.1\r\n" + host + b"Content-Length: 1\r\nContent-Length: 2\r\n\r\nxx",
           b"POST /api/clips HTTP/1.1\r\n" + host + b"Content-Length: 2\r\n\r\nxxxx"]
    codes = [status_of(srv.raw(r)) for r in bad]
    check(codes == [400] * len(bad), f"{len(bad)} malformed requests get 400", codes)
    check(status_of(srv.raw(b"GET /api/state HTTP/1.1\r\n" + host + b"X: " + b"a" * 20000 + b"\r\n\r\n")) == 431 and
          status_of(srv.raw(b"GET /" + b"a" * 20000 + b" HTTP/1.1\r\n" + host + b"\r\n")) == 431, "a request head over 16 KiB gets 431")
    check(status_of(srv.raw(b"POST /api/clips HTTP/1.1\r\n" + host + b"Content-Length: 2000000000\r\n\r\n")) == 413 and
          status_of(srv.raw(b"POST /api/clips HTTP/1.1\r\n" + host + b"Content-Length: 99999999999999999999\r\n\r\n")) == 413,
          "a body over 1 GiB is refused before it is read")
    check(status_of(srv.raw(b"POST /api/clips HTTP/1.1\r\n" + host + b"Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n")) == 501,
          "chunked uploads are not accepted")
    check(srv.raw(b"POST /api/clips HTTP/1.1\r\n" + host + b"Content-Length: 5000\r\n\r\nonly this") == b"" and
          srv.raw(b"GET /api/state HTTP/1.1\r\nHost") == b"" and srv.raw(b"") == b"",
          "a request that stops half way gets no answer")
    answer = srv.raw(b"POST /api/clips/1/open HTTP/1.1\r\n" + host + b"Expect: 100-continue\r\nContent-Length: 2\r\n\r\n{}")
    check(answer.startswith(b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 "), "Expect: 100-continue is honoured")
    answer = srv.raw(b"HEAD / HTTP/1.1\r\n" + host + b"\r\n")
    check(status_of(answer) == 200 and answer.endswith(b"\r\n\r\n") and b"Content-Length: " in answer, "HEAD gives the head only")

    # A connection that never sends anything (browsers open such connections) must not hold up the others.
    idle = [socket.create_connection(("127.0.0.1", srv.port)) for _ in range(6)]
    t0 = time.monotonic()
    ok = all(srv.alive() for _ in range(5))
    took = time.monotonic() - t0
    for s in idle:
        s.close()
    check(ok and took < 3.0, f"six idle connections do not hold up real requests ({took:.2f} s for five)")

    # Mutated requests: nothing may stop the service.
    rng = random.Random(606)
    seeds = [b"GET /api/state HTTP/1.1\r\n" + host + b"\r\n",
             b"POST /api/clips?name=a.webm HTTP/1.1\r\n" + host + b"Content-Length: 8\r\nContent-Type: application/octet-stream\r\n\r\n\x1aE\xdf\xa3\x01\x00\x00\x00",
             b"POST /api/clips/1/open HTTP/1.1\r\n" + host + b"Content-Length: 34\r\n\r\n" + b'{"schema":1,"alpha_mode":"opaque"}',
             b"GET /api/clips/1/frame?at_us=33000 HTTP/1.1\r\n" + host + b"Origin: http://127.0.0.1:" + str(srv.port).encode() + b"\r\n\r\n"]
    cases = 300
    for n in range(cases):
        data = bytearray(rng.choice(seeds))
        for _ in range(rng.randrange(1, 6)):
            kind = rng.randrange(5)
            at = rng.randrange(len(data)) if data else 0
            if kind == 0 and data:
                data[at] = rng.randrange(256)
            elif kind == 1 and data:
                del data[at:at + rng.randrange(1, 12)]
            elif kind == 2:
                data[at:at] = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 12)))
            elif kind == 3:
                data[at:at] = rng.choice([b"\r\n", b"\r\n\r\n", b"%", b"%00", b"Content-Length: 4294967296\r\n", b"/../", b"\x00", b" ", b":"])
            else:
                data = data[:at]
        srv.raw(bytes(data), timeout=3.0)
        if n % 50 == 49 and not srv.alive():
            break
    check(srv.alive(), f"{cases} mutated requests: the service still answers")
    status, state = srv.json("/api/state")
    check(status == 200 and "clip" in state, "... and its state is still readable")


# ---- the CPU reference of the shader and of the blend ---------------------------------------------------------------

COEFF = {"bt601": (1.402, 0.344136, 0.714136, 1.772), "bt709": (1.5748, 0.187324, 0.468124, 1.8556)}


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.maximum(c, 0.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def to_bytes(x):
    return np.rint(np.clip(x, 0.0, 1.0) * 255.0).astype(np.uint8)


def cpu_output(frame, info, matrix, full, shift, linear):
    """shaders/alphaclip_yuv_to_rgba.frag.glsl on the CPU: the bytes of the output target, rows top first."""
    w, h = info["width"], info["height"]
    cw, ch = (w + 1) // 2, (h + 1) // 2
    pw, ph = info["content_w"], info["content_h"]
    pcw, pch = (pw + 1) // 2, (ph + 1) // 2
    off, luma_scale, chroma_scale = (0.0, 1.0, 1.0) if full else (16 / 255, 255 / 219, 255 / 224)
    Y = frame.y.astype(np.float64) / 255
    U = frame.u.astype(np.float64) / 255
    V = frame.v.astype(np.float64) / 255
    y = (Y[:ph, :pw] - off) * luma_scale
    # the chroma lookup: bilinear, shifted, clamped to the picture's own chroma samples
    cx = np.clip((np.arange(pw) + 0.5) / w + shift / cw, 0.5 / cw, (pcw - 0.5) / cw) * cw - 0.5
    cy = np.clip((np.arange(ph) + 0.5) / h, 0.5 / ch, (pch - 0.5) / ch) * ch - 0.5
    x0 = np.clip(np.floor(cx).astype(int), 0, cw - 1)
    y0 = np.clip(np.floor(cy).astype(int), 0, ch - 1)
    fx = np.clip(cx - x0, 0, 1)[None, :]
    fy = np.clip(cy - y0, 0, 1)[:, None]
    x1 = np.minimum(x0 + 1, cw - 1)
    y1 = np.minimum(y0 + 1, ch - 1)

    def bilinear(P):
        top = P[y0][:, x0] * (1 - fx) + P[y0][:, x1] * fx
        bottom = P[y1][:, x0] * (1 - fx) + P[y1][:, x1] * fx
        return top * (1 - fy) + bottom * fy

    u = (bilinear(U) - 128 / 255) * chroma_scale
    v = (bilinear(V) - 128 / 255) * chroma_scale
    k = COEFF[matrix]
    rgb = np.clip(np.stack([y + k[0] * v, y - k[1] * u - k[2] * v, y + k[3] * u], axis=-1), 0, 1)
    mode, premultiplied = info["alpha_mode"], info["premultiplied"]
    if mode == 3:
        a = frame.a.astype(np.float64)[:ph, :pw] / 255
    elif mode in (1, 2):
        ax, ay = info["alpha_x"], info["alpha_y"]
        a = np.clip((Y[ay:ay + ph, ax:ax + pw] - off) * luma_scale, 0, 1)
    else:
        a = np.ones((ph, pw))
    a3 = a[..., None]
    if mode != 0:
        if linear:
            straight = np.clip(rgb / np.maximum(a3, 1e-4), 0, 1) if premultiplied else rgb
            rgb = srgb_to_linear(straight) * a3
        else:
            rgb = np.minimum(rgb, a3) if premultiplied else rgb * a3
    elif linear:
        rgb = srgb_to_linear(rgb)
    stored = linear_to_srgb(rgb) if linear else rgb
    return to_bytes(np.concatenate([stored, a3], axis=-1))


def cpu_scene(output, background, linear, blend="premultiplied"):
    """The output bytes drawn over a background colour: what the blend stage stores (RGB only)."""
    src = output.astype(np.float64) / 255
    a = src[..., 3:4]
    bg = np.array(background, dtype=np.float64) / 255
    c = src[..., :3]
    if linear:
        c, bg = srgb_to_linear(c), srgb_to_linear(bg)
    result = c * (a if blend == "straight" else 1.0) + bg * (1 - a)
    return to_bytes(linear_to_srgb(result) if linear else result)


def truth_stats(rendered, truth_frame, background, linear):
    """AlphaClipTruth.Measure: the composite against truth.rgb * a + background * (1 - a)."""
    t = truth_frame.astype(np.float64) / 255
    a = t[..., 3:4]
    bg = np.array(background, dtype=np.float64) / 255
    if linear:
        expected = linear_to_srgb(srgb_to_linear(t[..., :3]) * a + srgb_to_linear(bg) * (1 - a))
    else:
        expected = t[..., :3] * a + bg * (1 - a)
    err = rendered[..., :3].astype(np.int64) - np.rint(expected * 255).astype(np.int64)
    edge = (truth_frame[..., 3] >= 13) & (truth_frame[..., 3] <= 242)
    return {"count": err.size, "sum_abs": float(np.abs(err).sum()), "max": int(np.abs(err).max()),
            "edge_count": int(edge.sum()) * 3, "edge_sum": float(err[edge].sum())}


def add_stats(total, s):
    if total is None:
        return dict(s)
    out = {k: total[k] + s[k] for k in ("count", "sum_abs", "edge_count", "edge_sum")}
    out["max"] = max(total["max"], s["max"])
    return out


def diff(a, b):
    d = np.abs(a.astype(np.int64) - b.astype(np.int64))
    return int(d.max()), float(d.mean())


# ---- the browser ---------------------------------------------------------------------------------------------------

class Sink:
    """The collector autotest.js talks to: hands out the jobs and the clips, takes the pixels."""

    def __init__(self, jobs, files):
        self.jobs, self.files = jobs, files
        self.results = {}
        self.report = None
        self.done = threading.Event()
        sink = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def answer(self, status, body=b"", ctype="application/octet-stream"):
                self.send_response(status)
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
                self.send_header("Access-Control-Allow-Headers", "Content-Type")
                self.send_header("Access-Control-Allow-Private-Network", "true")
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_OPTIONS(self):
                self.answer(204)

            def do_GET(self):
                if self.path == "/jobs":
                    self.answer(200, json.dumps({"jobs": sink.jobs}).encode(), "application/json")
                elif self.path.startswith("/file/") and urllib.parse.unquote(self.path[6:]) in sink.files:
                    self.answer(200, read(sink.files[urllib.parse.unquote(self.path[6:])]))
                else:
                    self.answer(404)

            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                url = urllib.parse.urlsplit(self.path)
                q = dict(urllib.parse.parse_qsl(url.query))
                if url.path == "/result":
                    w, h = int(q["width"]), int(q["height"])
                    if len(body) == w * h * 4:
                        sink.results[q["name"]] = (np.frombuffer(body, dtype=np.uint8).reshape(h, w, 4), int(q["pts"]))
                    self.answer(200)
                elif url.path == "/done":
                    sink.report = json.loads(body)
                    self.answer(200)
                    sink.done.set()
                else:
                    self.answer(404)

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def run_browser(browser, extra, url, sink, timeout=240):
    """Opens the test page in a headless browser with its own empty profile and waits for the report."""
    # A fresh profile for every start: a profile whose browser was just ended by force may still be locked.
    run_browser.count = getattr(run_browser, "count", 0) + 1
    profile = work(f"browser-profile-{run_browser.count}", "x")[:-2]
    args = [browser, "--headless=new", "--user-data-dir=" + profile, "--no-first-run", "--no-default-browser-check",
            "--disable-extensions", "--disable-background-networking", "--disable-component-update", "--disable-sync",
            "--disable-default-apps", "--mute-audio",
            # Nothing but this machine: every other address goes to a proxy that does not exist.
            "--proxy-server=http://127.0.0.1:9", *extra, url]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        finished = sink.done.wait(timeout)
    finally:
        if os.name == "nt":
            subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"], capture_output=True)
            # Edge hands over to a second process of its own and lets the first one end, so the tree above can be
            # empty while the browser lives on. End whatever still runs with THIS start's profile folder, and nothing
            # else: the user's own browser has another profile and is never touched.
            script = ("$d = '" + profile.replace("'", "''") + "'; $n = '" + os.path.basename(browser).replace("'", "''") + "'; "
                      "Get-CimInstance Win32_Process | Where-Object { $_.Name -eq $n -and $_.ProcessId -ne $PID -and "
                      "$_.CommandLine -and $_.CommandLine.Contains($d) } | "
                      "ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }")
            subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script], capture_output=True)
        else:
            proc.terminate()
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()
    return finished


def frame_time(n, fps=30):
    """A time inside frame n, as the Unity tests pick it (AlphaClipTruth.ShowFrame)."""
    return int(round(n * 1000.0 / fps)) * 1000 + 500


def flipped(meta):
    out = meta.replace('"premultiplied": true', '"premultiplied": FALSE').replace('"premultiplied": false', '"premultiplied": true')
    out = out.replace('"premultiplied": FALSE', '"premultiplied": false')
    assert out != meta
    return out


def bg_name(bg):
    return "bg%d_%d_%d" % bg


def build_jobs():
    jobs, files = [], {}

    def job(name, clip_path, meta, at_us, linear, want_output=True, scene=None, matrix="bt709", full=False, shift=0.25):
        files[os.path.basename(clip_path)] = clip_path
        jobs.append({"name": name, "clip": os.path.basename(clip_path), "meta": meta, "at_us": at_us, "linear": linear,
                     "default_matrix": matrix, "default_full_range": full, "chroma_shift": shift, "want_output": want_output, "scene": scene})

    for clip in M4:
        path = sample("m4", clip + ".webm")
        meta = read(sidecar_of(path), "r")
        for cs in ("gamma", "linear"):
            linear = cs == "linear"
            for n in TRUTH_FRAMES:
                for i, bg in enumerate(BACKGROUNDS):
                    job(f"{clip}.f{n}.{cs}.{bg_name(bg)}", path, meta, frame_time(n), linear, want_output=(i == 0),
                        scene={"bg": bg, "blend": "premultiplied"})
                job(f"{clip}.f{n}.{cs}.straight", path, meta, frame_time(n), linear, want_output=False,
                    scene={"bg": BACKGROUNDS[0], "blend": "straight"})
            for n in TRUTH_FRAMES:
                job(f"{clip}.f{n}.{cs}.flag", path, flipped(meta), frame_time(n), linear, scene={"bg": BACKGROUNDS[0], "blend": "premultiplied"})
    # No sidecar: the page's defaults decide (both matrices, both ranges, both chroma positions).
    opaque = sample("m0", "opaque.webm")
    for cs in ("gamma", "linear"):
        linear = cs == "linear"
        for tag, kw in (("bt709", {}), ("bt601", {"matrix": "bt601"}), ("full", {"full": True}), ("centre", {"shift": 0.0}),
                        ("bt601full", {"matrix": "bt601", "full": True})):
            job(f"opaque.f12.{cs}.{tag}", opaque, None, frame_time(12), linear, scene={"bg": (128, 128, 128), "blend": "premultiplied"}, **kw)
        for name, folder, n in (("alpha_native", "m0", 20), ("alpha_odd", "m1", 7), ("opaque_odd", "m1", 7), ("opaque_altref", "m1", 9),
                                ("alpha_scenecut", "m1", 31)):
            job(f"{name}.f{n}.{cs}.auto", sample(folder, name + ".webm"), None, frame_time(n), linear,
                scene={"bg": (0, 200, 230), "blend": "premultiplied"})
        # magnified: nearest must repeat the pixels, bilinear must stay between its neighbours
        path = sample("m4", "native_premul.webm")
        meta = read(sidecar_of(path), "r")
        for filt in ("nearest", "linear"):
            job(f"native_premul.f20.{cs}.zoom2{filt}", path, meta, frame_time(20), linear,
                scene={"bg": (128, 128, 128), "blend": "premultiplied", "zoom": 2, "filter": filt})
    return jobs, files


def part_b(args, truth):
    print("== B: the page's drawing code in a browser")
    jobs, files = build_jobs()
    sink = Sink(jobs, files)
    srv = Service()
    try:
        finished = run_browser(args.browser, args.browser_arg, srv.url + "/autotest.html?sink=" + urllib.parse.quote(sink.url, safe=""), sink)
        report = sink.report or {}
        check(finished and report.get("fatal") is None, "the test page ran to the end", report.get("fatal") or "no report (timeout)")
        if not finished:
            return None
        gl = report.get("gl") or {}
        print(f"INFO  browser: {report.get('userAgent')}")
        print(f"INFO  WebGL: {gl.get('version')} | {gl.get('renderer')}")
        check(report.get("errors") == [] and report.get("jobs") == len(jobs), f"all {len(jobs)} jobs were drawn", report.get("errors"))
        infos = {}
        for name in files:
            cid = srv.upload(name, read(files[name]))
            for meta in {j["meta"] for j in jobs if j["clip"] == name}:
                status, opened = srv.open(cid, meta)
                frames = {}
                for j in jobs:
                    if j["clip"] == name and j["meta"] == meta and j["at_us"] not in frames:
                        frames[j["at_us"]] = Frame(srv.frame(cid, at_us=j["at_us"])[1])
                infos[(name, meta)] = (opened["info"], frames)
    finally:
        srv.quit()
        sink.close()
    R = sink.results

    # B1 the shader against the CPU implementation of the same formulas
    worst = {"gamma": (0, 0.0, ""), "linear": (0, 0.0, "")}
    n_out = 0
    for j in jobs:
        if not j["want_output"] or j["name"] + ".output" not in R:
            continue
        info, frames = infos[(j["clip"], j["meta"])]
        out, pts = R[j["name"] + ".output"]
        frame = frames[j["at_us"]]
        matrix = "bt709" if info["matrix"] == 2 else "bt601" if info["matrix"] == 1 else j["default_matrix"]
        full = info["color_range"] == 2 or (info["color_range"] != 1 and j["default_full_range"])
        ref = cpu_output(frame, info, matrix, full, j["chroma_shift"], j["linear"])
        cs = "linear" if j["linear"] else "gamma"
        if out.shape != ref.shape or pts != frame.pts:
            check(False, f"{j['name']}: the output has the picture's size and the right frame", (out.shape, ref.shape, pts, frame.pts))
            continue
        mx, mean = diff(out, ref)
        n_out += 1
        if mx > worst[cs][0] or (mx == worst[cs][0] and mean > worst[cs][1]):
            worst[cs] = (mx, mean, j["name"])
    for cs in ("gamma", "linear"):
        check(worst[cs][0] <= 1, f"shader output = CPU reference within 1 level, {cs} (worst: max {worst[cs][0]}, mean {worst[cs][1]:.4f}, {worst[cs][2]})")
    check(n_out == sum(1 for j in jobs if j["want_output"]), f"{n_out} outputs compared (every alpha mode, matrix, range, chroma position, odd sizes)")
    packed = R["packed_right_premul.f0.gamma." + bg_name(BACKGROUNDS[0]) + ".output"][0]
    check(packed.shape == (TRUTH_H, TRUTH_W, 4), "a packed clip's output has the picture's size, not the coded frame's", packed.shape)

    # B2 the blend against the CPU blend of the shader's output
    # Gamma: plain arithmetic on bytes, within 1 level everywhere. Linear: the hardware decodes sRGB, blends and
    # encodes again, and near black the curve is so steep (one step of linear light is several levels) that graphics
    # cards disagree with each other there: an RTX 4060 is 2 levels off on up to 3 values per picture (blue = 2 at
    # alpha 254 over white: 16 instead of 14). So Linear allows 2 levels on at most one value in 10,000.
    last_output = None
    worst = {"gamma": (0, ""), "linear": (0, "")}
    over_one = {"gamma": 0, "linear": 0}
    values = {"gamma": 0, "linear": 0}
    n_scene = 0
    for j in jobs:
        if j["name"] + ".output" in R:
            last_output = R[j["name"] + ".output"][0]
        scene = j["scene"]
        if not scene or scene.get("zoom", 1) != 1 or j["name"] + ".scene" not in R:
            continue
        got = R[j["name"] + ".scene"][0]
        ref = cpu_scene(last_output, scene["bg"], j["linear"], scene["blend"])
        d = np.abs(got[..., :3].astype(np.int64) - ref.astype(np.int64))
        n_scene += 1
        cs = "linear" if j["linear"] else "gamma"
        over_one[cs] += int((d > 1).sum())
        values[cs] += d.size
        if int(d.max()) > worst[cs][0]:
            worst[cs] = (int(d.max()), j["name"])
    check(worst["gamma"][0] <= 1, f"composite = CPU blend of the output within 1 level, gamma (worst: max {worst['gamma'][0]} {worst['gamma'][1]})")
    share = over_one["linear"] / max(values["linear"], 1)
    check(worst["linear"][0] <= 2 and share <= 1e-4,
          f"composite = CPU blend of the output, linear: at most 2 levels, and more than 1 on {over_one['linear']} of {values['linear']} values "
          f"({share:.1e} <= 1e-4; worst: max {worst['linear'][0]} {worst['linear'][1]})")
    check(n_scene > 100, f"{n_scene} composites compared")

    # B3 against the exact ground truth, with the controls
    for cs in ("gamma", "linear"):
        linear = cs == "linear"
        for clip in M4:
            for bg in BACKGROUNDS:
                total = None
                for f, n in enumerate(TRUTH_FRAMES):
                    total = add_stats(total, truth_stats(R[f"{clip}.f{n}.{cs}.{bg_name(bg)}.scene"][0], truth[f], bg, linear))
                mean_abs, edge = total["sum_abs"] / total["count"], total["edge_sum"] / total["edge_count"]
                print(f"INFO  {clip} over {bg} [{cs}]: mean |error| {mean_abs:.3f}, max {total['max']}, soft-edge signed mean {edge:.3f} over {total['edge_count']} samples")
                check(total["edge_count"] > 100 and mean_abs <= MAX_MEAN_ABS and abs(edge) <= MAX_EDGE_MEAN,
                      f"{clip} over {bg} [{cs}]: composite matches the truth, no rim (mean |error| {mean_abs:.2f} <= {MAX_MEAN_ABS}, soft-edge signed mean {edge:.2f}, limit +-{MAX_EDGE_MEAN})")
            for control, want_dark in (("straight", True), ("flag", False)):
                total = None
                for f, n in enumerate(TRUTH_FRAMES):
                    total = add_stats(total, truth_stats(R[f"{clip}.f{n}.{cs}.{control}.scene"][0], truth[f], BACKGROUNDS[0], linear))
                edge = total["edge_sum"] / total["edge_count"]
                print(f"INFO  {clip} CONTROL {control} over white [{cs}]: soft-edge signed mean {edge:.2f}")
                if want_dark:
                    check(edge < -MIN_CONTROL[cs], f"{clip} [{cs}]: control, straight-alpha blending gives a dark rim ({edge:.1f} < -{MIN_CONTROL[cs]})")
                else:
                    check(abs(edge) > MIN_CONTROL[cs], f"{clip} [{cs}]: control, the wrong premultiplied flag is seen ({edge:.1f}, |.| > {MIN_CONTROL[cs]})")

    # B4 magnified
    for cs in ("gamma", "linear"):
        base = R[f"native_premul.f20.{cs}.{bg_name(BACKGROUNDS[1])}.scene"][0][..., :3]
        near = R[f"native_premul.f20.{cs}.zoom2nearest.scene"][0][..., :3]
        lin = R[f"native_premul.f20.{cs}.zoom2linear.scene"][0][..., :3]
        check(near.shape == (2 * TRUTH_H, 2 * TRUTH_W, 3) and np.array_equal(near, np.repeat(np.repeat(base, 2, axis=0), 2, axis=1)),
              f"magnified 2x with the nearest filter: every pixel twice [{cs}]")
        pad = np.pad(base.astype(np.int64), ((1, 1), (1, 1), (0, 0)), mode="edge")
        lo = np.minimum.reduce([pad[dy:dy + TRUTH_H, dx:dx + TRUTH_W] for dy in range(3) for dx in range(3)])
        hi = np.maximum.reduce([pad[dy:dy + TRUTH_H, dx:dx + TRUTH_W] for dy in range(3) for dx in range(3)])
        up = lambda p: np.repeat(np.repeat(p, 2, axis=0), 2, axis=1)
        inside = (lin.astype(np.int64) >= up(lo) - 1) & (lin.astype(np.int64) <= up(hi) + 1)
        check(lin.shape == near.shape and bool(inside.all()) and not np.array_equal(lin, near),
              f"magnified 2x with the bilinear filter: every pixel lies between its neighbours [{cs}]", float(inside.mean()))
    return R, gl


# ---- against Unity -------------------------------------------------------------------------------------------------

def write_png(path, rgb):
    h, w, _ = rgb.shape
    rows = b"".join(b"\x00" + rgb[r].tobytes() for r in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def part_c(R, gl, folder, cs):
    print(f"== C: against Unity [{cs}]")
    info_path = os.path.join(folder, "info.txt")
    check(os.path.exists(info_path), f"the Unity dump {folder} has an info.txt")
    if not os.path.exists(info_path):
        return
    unity_info = read(info_path, "r").strip().replace("\n", " | ")
    print(f"INFO  Unity: {unity_info}")
    check(("colour space: " + cs) in unity_info.lower(), f"the dump was made in a {cs} project")
    # The same graphics card on both sides is the real comparison. On another card the Linear blend differs by itself
    # near black (see B2): there a few values may be further apart, and the check says so.
    card = re.search(r"graphics: (.*?) \(", unity_info)
    same_card = bool(card) and card.group(1) in (gl.get("renderer") or "")
    print(f"INFO  the browser draws on {'the same' if same_card else 'ANOTHER'} graphics card as Unity")
    strict = same_card or cs == "gamma"
    groups = {"output": [], "scene": [], "control": []}
    missing = []
    for name in sorted(os.listdir(folder)):
        if not name.endswith(".rgba"):
            continue
        # Unity: <clip>.f<n>.<what>.<output|scene>.rgba   preview: <clip>.f<n>.<cs>.<what>.<output|scene>
        clip, frame, what, kind = name[:-5].split(".")
        key = f"{clip}.{frame}.{cs}.{what}.{kind}"
        if key not in R:
            missing.append(key)
            continue
        mine = R[key][0]
        theirs = np.frombuffer(read(os.path.join(folder, name)), dtype=np.uint8)
        if theirs.size != mine.size:
            missing.append(key + " (size)")
            continue
        theirs = theirs.reshape(mine.shape)
        channels = slice(0, 4) if kind == "output" else slice(0, 3)   # a composite's alpha channel is not part of the picture
        mx, mean = diff(mine[..., channels], theirs[..., channels])
        over = int((np.abs(mine[..., channels].astype(np.int64) - theirs[..., channels].astype(np.int64)) > UNITY_MAX).sum())
        groups["control" if what in ("straight", "flag") else kind].append((mx, mean, key, over, mine[..., channels].size))
        if kind == "scene" and frame == "f20" and what in (bg_name(BACKGROUNDS[2]), "flag"):
            d = np.abs(mine[..., :3].astype(np.int64) - theirs[..., :3].astype(np.int64))
            gap = np.full((mine.shape[0], 4, 3), 40, dtype=np.uint8)
            sheet = np.concatenate([theirs[..., :3], gap, mine[..., :3], gap, np.clip(d * 64, 0, 255).astype(np.uint8)], axis=1)
            write_png(work("compare", f"{cs}_{clip}_{what}_unity_preview_diffx64.png"), sheet)
    check(not missing, "every picture Unity dumped has its counterpart from the preview", missing[:5])
    for group, label, want in (("output", "the shader's output (premultiplied RGBA)", 12), ("scene", "the composite over three backgrounds", 36),
                               ("control", "the two controls (wrong flag: output and composite; straight-alpha shader)", 36)):
        rows = groups[group]
        if not rows:
            check(False, f"Unity [{cs}]: {label}: nothing to compare")
            continue
        mx = max(r[0] for r in rows)
        mean = sum(r[1] for r in rows) / len(rows)
        worst = max(rows)[2]
        over, total = sum(r[3] for r in rows), sum(r[4] for r in rows)
        print(f"INFO  Unity [{cs}] {label}: {len(rows)} pictures, largest difference {mx} (in {worst}), mean {mean:.4f}, "
              f"more than {UNITY_MAX} on {over} of {total} values")
        if strict:
            check(len(rows) == want and mx <= UNITY_MAX and mean <= UNITY_MEAN,
                  f"preview = Unity, {label} [{cs}]: {len(rows)} pictures, max {mx} <= {UNITY_MAX}, mean {mean:.4f} <= {UNITY_MEAN}")
        else:
            check(len(rows) == want and mx <= 4 and over <= 1e-4 * total and mean <= UNITY_MEAN,
                  f"preview on another graphics card ~ Unity, {label} [{cs}]: {len(rows)} pictures, max {mx} <= 4, more than {UNITY_MAX} "
                  f"on {over} of {total} values (<= 1 in 10,000), mean {mean:.4f} <= {UNITY_MEAN}")


def part_d(args):
    print("== D: the page itself, used as a person would")
    first = sample("m4", "native_premul.webm")
    garbage, override = work("d", "garbage.webm"), work("d", "opaque_override.json")
    with open(garbage, "wb") as f:
        f.write(b"this is not a video " * 200)
    with open(override, "w") as f:
        f.write('{"schema":1,"alpha_mode":"opaque"}')
    packed = sample("m4", "packed_bottom_straight.webm")
    files = {"packed_bottom_straight.webm": packed, "packed_bottom_straight.acmeta.json": sidecar_of(packed),
             "opaque_override.json": override, "alpha_native.webm": sample("m0", "alpha_native.webm"), "garbage.webm": garbage,
             "native_premul.webm": first, "native_premul.acmeta.json": sidecar_of(first)}
    sink = Sink([], files)
    srv = Service(first)
    try:
        finished = run_browser(args.browser, args.browser_arg, srv.url + "/uitest.html?sink=" + urllib.parse.quote(sink.url, safe=""), sink, timeout=180)
        report = sink.report or {}
        check(finished and report.get("fatal") is None, "the page test ran to the end", report.get("fatal") or "no report (timeout)")
        items = report.get("ui", [])
        for item in items:
            check(item["ok"], "page: " + item["label"], item["detail"])
        check(len(items) >= 30, f"{len(items)} things were tried on the page")
        check(report.get("errors") == [], "no script error on the page", report.get("errors"))
        # what the page had on screen = the CPU reference of the same frame
        cid = srv.upload("native_premul.webm", read(first))
        status, opened = srv.open(cid, read(sidecar_of(first), "r"))
        frame = Frame(srv.frame(cid, at_us=frame_time(20))[1])
        for cs in ("gamma", "linear"):
            got = sink.results.get(f"ui.f20.{cs}.white")
            if got is None or got[0].shape != (TRUTH_H, TRUTH_W, 4) or got[1] != frame.pts:
                check(False, f"the page's own picture of frame 20 arrived [{cs}]", None if got is None else (got[0].shape, got[1]))
                continue
            ref = cpu_scene(cpu_output(frame, opened["info"], "bt601", False, 0.25, cs == "linear"), (255, 255, 255), cs == "linear")
            mx, mean = diff(got[0][..., :3], ref)
            check(mx <= 3 and mean <= 0.1, f"what the page shows at 100% over white = CPU reference [{cs}] (max {mx}, mean {mean:.4f})")
    finally:
        srv.quit()
        sink.close()


def main():
    global CLI, WORK
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--truth", default=sample("m4", "truth_rgba_3f.raw"))
    ap.add_argument("--browser", default="")
    ap.add_argument("--browser-arg", action="append", default=[])
    ap.add_argument("--unity-gamma", default="")
    ap.add_argument("--unity-linear", default="")
    ap.add_argument("--only", default="")
    args = ap.parse_args()
    CLI = os.path.abspath(args.cli)
    WORK = os.path.abspath(args.workdir)
    os.makedirs(WORK, exist_ok=True)
    only = args.only.upper()

    truth = np.fromfile(args.truth, dtype=np.uint8).reshape(len(TRUTH_FRAMES), TRUTH_H, TRUTH_W, 4)
    if not only or "A" in only:
        part_a()
    drawn = None
    if not only or "B" in only or "C" in only:
        if not args.browser:
            print("SKIP  parts B, C and D: no --browser given")
        else:
            drawn = part_b(args, truth)
    if drawn is not None and (not only or "C" in only):
        R, gl = drawn
        if args.unity_gamma:
            part_c(R, gl, args.unity_gamma, "gamma")
        if args.unity_linear:
            part_c(R, gl, args.unity_linear, "linear")
        if not args.unity_gamma and not args.unity_linear:
            print("SKIP  part C: no --unity-gamma / --unity-linear dump given")
    if args.browser and (not only or "D" in only):
        part_d(args)
    print(f"\n{checks - failures}/{checks} checks passed")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
