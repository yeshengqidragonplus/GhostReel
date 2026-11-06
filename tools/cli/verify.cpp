// `alphaclip verify <clip.webm> [--meta <json> | --no-meta] [--ffmpeg <path>] [--no-ffmpeg]` (design doc TR-3, risks R9
// and R10).
//
// Checks that a clip is fit to ship, prints one line per check and exits 0 (nothing failed; warnings do not count) or
// 1. The checks, in order:
//   container   the file is WebM, VP9, has packets
//   timestamps  strictly increasing; a warning when the frame time varies (looping assumes a constant one)
//   keyframes   the first packet is a keyframe on every stream; a warning for packets that are a keyframe on only one
//               of the colour / alpha streams (the core can only seek where both are); the longest gap between seek points
//   cues        the file has Cues for the video track and every cue points at a packet where every stream has a keyframe
//   sidecar     <clip>.acmeta.json is there and says which colour matrix / range to use (warnings otherwise)
//   open        the core opens the clip with that sidecar
//   decode      every frame decodes
//   alpha       the alpha range actually used; warnings for an alpha that never changes and for a "premultiplied" flag
//               that contradicts the picture
//   seek        seeking to every seek point and into the middle of every gap returns the frame that sequential decoding
//               shows at that time
//   ffmpeg      the core's frames equal an independent ffmpeg (libvpx-vp9) decode, byte for byte
// Everything except the last check works without ffmpeg.
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "alphaclip.h"
#include "analyze.h"
#include "commands.h"
#include "common.h"
#include "util.h"

namespace cli {
namespace {

std::string Fmt(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return buf;
}

struct Report {
  int fails = 0;
  int warns = 0;
  void Ok(const char* name, const std::string& text) const { std::printf("[ ok ] %s: %s\n", name, text.c_str()); }
  void Warn(const char* name, const std::string& text) {
    ++warns;
    std::printf("[warn] %s: %s\n", name, text.c_str());
  }
  void Fail(const char* name, const std::string& text) {
    ++fails;
    std::printf("[FAIL] %s: %s\n", name, text.c_str());
  }
  void Skip(const char* name, const std::string& text) const { std::printf("[skip] %s: %s\n", name, text.c_str()); }
  int Finish() const {
    if (fails == 0) std::printf("result: PASS (%d warning%s)\n", warns, warns == 1 ? "" : "s");
    else std::printf("result: FAIL (%d failure%s, %d warning%s)\n", fails, fails == 1 ? "" : "s", warns, warns == 1 ? "" : "s");
    std::fflush(stdout);
    return fails == 0 ? 0 : 1;
  }
};

struct Player {
  ac_player* p = nullptr;
  ~Player() { ac_close(p); }
};

const char* ModeName(int mode) {
  switch (mode) {
    case AC_ALPHA_OPAQUE: return "opaque";
    case AC_ALPHA_PACKED_RIGHT: return "packed_right";
    case AC_ALPHA_PACKED_BOTTOM: return "packed_bottom";
    case AC_ALPHA_NATIVE: return "native_alpha";
    default: return "unknown";
  }
}

uint64_t HashFrame(const ac_frame& f) {
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  const int w = f.width;
  const int h2 = f.height;
  const int cw = (w + 1) / 2;
  const int ch = (h2 + 1) / 2;
  const int pw[4] = {w, cw, cw, w};
  const int ph[4] = {h2, ch, ch, h2};
  for (int k = 0; k < 4; ++k) {
    if (f.plane[k] == nullptr) continue;
    for (int y = 0; y < ph[k]; ++y) {
      const uint8_t* row = f.plane[k] + static_cast<ptrdiff_t>(y) * f.stride[k];
      for (int x = 0; x < pw[k]; ++x) {
        h ^= row[x];
        h *= 1099511628211ull;
      }
    }
  }
  return h;
}

// Byte-for-byte comparison of a core frame with one raw frame from ffmpeg (Y, U, V[, A], tightly packed).
bool SameAsReference(const ac_frame& f, const uint8_t* ref, std::string* where) {
  const int w = f.width;
  const int h = f.height;
  const int cw = (w + 1) / 2;
  const int ch = (h + 1) / 2;
  const struct {
    int k, pw, ph;
    const char* name;
  } planes[4] = {{0, w, h, "Y"}, {1, cw, ch, "U"}, {2, cw, ch, "V"}, {3, w, h, "A"}};
  size_t off = 0;
  for (const auto& pl : planes) {
    if (f.plane[pl.k] == nullptr) break;
    for (int y = 0; y < pl.ph; ++y) {
      const uint8_t* a = f.plane[pl.k] + static_cast<ptrdiff_t>(y) * f.stride[pl.k];
      const uint8_t* b = ref + off + static_cast<size_t>(y) * static_cast<size_t>(pl.pw);
      if (std::memcmp(a, b, static_cast<size_t>(pl.pw)) == 0) continue;
      int x = 0;
      while (a[x] == b[x]) ++x;
      *where = Fmt("plane %s at (%d,%d): core %d, ffmpeg %d", pl.name, x, y, a[x], b[x]);
      return false;
    }
    off += static_cast<size_t>(pl.pw) * static_cast<size_t>(pl.ph);
  }
  return true;
}

struct AlphaStats {
  int min = 255;
  int max = 0;
  uint64_t transparent = 0;       // pixels with alpha <= 2
  uint64_t transparent_dark = 0;  // ... whose luma is black (or nearly)
  bool any = false;
};

// Looks at the alpha the clip really uses: the A plane (native_alpha) or the luma of the alpha image (packed).
void ScanAlpha(const ac_frame& f, const ac_info& info, AlphaStats* s) {
  const bool full = info.color_range == AC_RANGE_FULL;
  const int black = full ? 0 : 16;
  const bool native = info.alpha_mode == AC_ALPHA_NATIVE;
  if (native && f.plane[3] == nullptr) return;
  s->any = true;
  for (int y = 0; y < info.content_h; ++y) {
    for (int x = 0; x < info.content_w; ++x) {
      const int luma = f.plane[0][static_cast<ptrdiff_t>(y) * f.stride[0] + x];
      int a;
      if (native) {
        a = f.plane[3][static_cast<ptrdiff_t>(y) * f.stride[3] + x];
      } else {
        const int v = f.plane[0][static_cast<ptrdiff_t>(info.alpha_y + y) * f.stride[0] + info.alpha_x + x];
        a = full ? v : std::min(255, std::max(0, ((v - 16) * 255 + 109) / 219));
      }
      s->min = std::min(s->min, a);
      s->max = std::max(s->max, a);
      if (a <= 2) {
        ++s->transparent;
        if (luma <= black + 8) ++s->transparent_dark;
      }
    }
  }
}

}  // namespace

int RunVerify(const VerifyOptions& o) {
  Report r;
  std::printf("alphaclip verify: %s\n", o.clip_path.c_str());

  std::vector<uint8_t> data;
  std::string error;
  if (!ReadFile(o.clip_path, &data, &error)) {
    r.Fail("file", error);
    return r.Finish();
  }

  // ---- container -------------------------------------------------------------------------------------------------
  ac::ClipAnalysis a;
  try {
    a = ac::AnalyzeWebm(data.data(), data.size());
  } catch (const ac::Failure& f) {
    r.Fail("container", f.what());
    return r.Finish();
  }
  const size_t n = a.packets.size();
  if (a.codec_id != "V_VP9") {
    r.Fail("container", "the video codec is '" + a.codec_id + "'; only V_VP9 is supported");
    return r.Finish();
  }
  if (n == 0) {
    r.Fail("container", "the video track has no packets");
    return r.Finish();
  }
  r.Ok("container", Fmt("WebM, V_VP9 %dx%d, %zu packets in %zu clusters, %zu bytes", a.width, a.height, n,
                        a.cluster_count, data.size()));

  // ---- timestamps ------------------------------------------------------------------------------------------------
  double fps = 0;
  {
    int64_t min_step = std::numeric_limits<int64_t>::max();
    int64_t max_step = 0;
    size_t bad = 0;
    for (size_t i = 1; i < n && bad == 0; ++i) {
      const int64_t step = a.packets[i].pts_us - a.packets[i - 1].pts_us;
      if (step <= 0) {
        bad = i;
        break;
      }
      min_step = std::min(min_step, step);
      max_step = std::max(max_step, step);
    }
    if (bad != 0) {
      r.Fail("timestamps", Fmt("packet %zu (pts %lld us) is not later than packet %zu (pts %lld us)", bad,
                               static_cast<long long>(a.packets[bad].pts_us), bad - 1,
                               static_cast<long long>(a.packets[bad - 1].pts_us)));
    } else if (n == 1) {
      r.Ok("timestamps", "a single packet");
    } else {
      fps = 1e6 * static_cast<double>(n - 1) / static_cast<double>(a.packets.back().pts_us - a.packets.front().pts_us);
      if (max_step > min_step + min_step / 2) {
        r.Warn("timestamps", Fmt("the frame time varies (steps of %lld..%lld us); looping and the loop period assume a "
                                 "constant frame time",
                                 static_cast<long long>(min_step), static_cast<long long>(max_step)));
      } else {
        r.Ok("timestamps", Fmt("strictly increasing, steps of %lld..%lld us (%.3f fps)%s",
                               static_cast<long long>(min_step), static_cast<long long>(max_step), fps,
                               a.packets.front().pts_us != 0 ? ", the first packet is not at 0" : ""));
      }
    }
  }

  // ---- keyframes -------------------------------------------------------------------------------------------------
  {
    size_t color_keys = 0, alpha_keys = 0, seekable = 0, one_sided = 0, first_one_sided = 0;
    size_t longest_gap = 0, last_seekable = 0;
    bool have_seekable = false;
    for (size_t i = 0; i < n; ++i) {
      const ac::PacketFacts& f = a.packets[i];
      if (f.color_key) ++color_keys;
      if (f.alpha_key) ++alpha_keys;
      if (a.stream_has_alpha && f.color_key != f.alpha_key) {
        if (one_sided++ == 0) first_one_sided = i;
      }
      if (!f.seekable) continue;
      ++seekable;
      if (have_seekable) longest_gap = std::max(longest_gap, i - last_seekable);
      last_seekable = i;
      have_seekable = true;
    }
    if (have_seekable) longest_gap = std::max(longest_gap, n - last_seekable);
    if (!a.packets[0].seekable) {
      r.Fail("keyframes", a.stream_has_alpha ? "the first packet is not a keyframe on both the colour and the alpha stream"
                                             : "the first packet is not a keyframe");
    } else if (one_sided != 0) {
      r.Warn("keyframes",
             Fmt("%zu packets are a keyframe on only one of the two streams (the first is packet %zu); seeking can only "
                 "start at the %zu packets where both are, the longest gap between them is %zu frames",
                 one_sided, first_one_sided, seekable, longest_gap));
    } else {
      r.Ok("keyframes", Fmt("%zu colour, %zu alpha, %zu seek points; the longest gap between seek points is %zu frames%s",
                            color_keys, alpha_keys, seekable, longest_gap,
                            fps > 0 ? Fmt(" (%.2f s)", static_cast<double>(longest_gap) / fps).c_str() : ""));
    }
  }

  // ---- cues ------------------------------------------------------------------------------------------------------
  if (a.cues.empty()) {
    r.Fail("cues", "the file has no Cues for the video track, so other players cannot seek in it (re-encode it with "
                   "`alphaclip encode`, which writes them)");
  } else {
    size_t bad = 0;
    const ac::CueFact* first_bad = nullptr;
    for (const ac::CueFact& c : a.cues) {
      if (c.problem.empty()) continue;
      if (bad++ == 0) first_bad = &c;
    }
    if (bad != 0) {
      r.Fail("cues", Fmt("%zu of %zu cue points are wrong; the first, at %lld us: %s", bad, a.cues.size(),
                         static_cast<long long>(first_bad->time_us), first_bad->problem.c_str()));
    } else {
      r.Ok("cues", Fmt("%zu cue points, each at a packet where every stream has a keyframe", a.cues.size()));
    }
  }

  // ---- sidecar and open ------------------------------------------------------------------------------------------
  std::string meta_text;
  bool have_meta = false;
  std::string meta_label;
  if (o.has_meta_text) {
    meta_text = o.meta_text;
    have_meta = true;
    meta_label = "the sidecar given by the caller";
  } else if (o.no_meta) {
    meta_label = "no sidecar (--no-meta)";
  } else if (!o.meta_path.empty()) {
    if (!ReadTextFile(o.meta_path, &meta_text, &error)) {
      r.Fail("sidecar", error);
      return r.Finish();
    }
    have_meta = true;
    meta_label = o.meta_path;
  } else {
    const std::string candidate = SidecarPathFor(o.clip_path);
    if (FileExists(candidate)) {
      if (!ReadTextFile(candidate, &meta_text, &error)) {
        r.Fail("sidecar", error);
        return r.Finish();
      }
      have_meta = true;
      meta_label = candidate;
    } else {
      meta_label = "no sidecar";
      r.Warn("sidecar", Fmt("there is no %s: the core opens the clip in 'auto' mode and every binding uses its own "
                            "colour matrix and range",
                            FileNameOf(candidate).c_str()));
    }
  }

  Player player;
  const ac_status open_status =
      ac_open_memory(data.data(), data.size(), have_meta ? meta_text.c_str() : nullptr, &player.p);
  if (open_status != AC_OK) {
    r.Fail("open", Fmt("the core refuses to open the clip (%s): %s", meta_label.c_str(), ac_last_error(nullptr)));
    return r.Finish();
  }
  ac_info info;
  std::memset(&info, 0, sizeof(info));
  if (ac_get_info(player.p, &info) != AC_OK) {
    r.Fail("open", ac_last_error(player.p));
    return r.Finish();
  }
  r.Ok("open", Fmt("read as %s, picture %dx%d in a %dx%d frame (%s)", ModeName(info.alpha_mode), info.content_w,
                   info.content_h, info.width, info.height, meta_label.c_str()));
  if (have_meta && (info.color_range == AC_RANGE_UNSPECIFIED || info.matrix == AC_MATRIX_UNSPECIFIED)) {
    r.Warn("sidecar", "it does not say color_range and matrix: every binding falls back to its own setting, so the "
                      "colours depend on the project");
  }

  // ---- decode (+ alpha statistics + the reference decode) -------------------------------------------------------
  const size_t w = static_cast<size_t>(info.width);
  const size_t h = static_cast<size_t>(info.height);
  const size_t frame_bytes = w * h + 2 * ((w + 1) / 2) * ((h + 1) / 2) + (info.has_alpha_plane ? w * h : 0);

  Child ffmpeg;
  bool ref_on = false;
  std::string ffmpeg_version;
  std::string ref_note;  // why the comparison does not run
  if (o.no_ffmpeg) {
    ref_note = "not requested (--no-ffmpeg)";
  } else {
    const std::string exe = FfmpegPath(o.ffmpeg);
    std::string out;
    std::string why;
    if (!RunCapture({exe, "-hide_banner", "-version"}, &out, &why)) {
      ref_note = why;
    } else {
      const size_t eol = out.find_first_of("\r\n");
      ffmpeg_version = out.substr(0, eol);
      const size_t copyright = ffmpeg_version.find(" Copyright");
      if (copyright != std::string::npos) ffmpeg_version.resize(copyright);
      const std::vector<std::string> args = {exe, "-nostdin", "-v", "error", "-c:v", "libvpx-vp9", "-i", o.clip_path,
                                             "-map", "0:v:0", "-fps_mode", "passthrough", "-pix_fmt",
                                             info.has_alpha_plane ? "yuva420p" : "yuv420p", "-f", "rawvideo", "-"};
      if (ffmpeg.Start(args, /*capture_stdout=*/true, &why)) ref_on = true;
      else ref_note = why;
    }
  }

  std::vector<uint64_t> hashes;
  std::vector<int64_t> frame_pts;
  std::vector<uint8_t> ref_frame(ref_on ? frame_bytes : 0);
  AlphaStats alpha;
  size_t ref_bad_frames = 0;
  std::string ref_first_bad;
  std::string ref_problem;  // the reference stopped early
  std::string decode_error;
  for (;;) {
    ac_frame f;
    const ac_status s = ac_decode_next(player.p, &f);
    if (s == AC_END) break;
    if (s != AC_OK) {
      decode_error = Fmt("after %zu frames: %s", hashes.size(), ac_last_error(player.p));
      break;
    }
    hashes.push_back(HashFrame(f));
    frame_pts.push_back(f.pts_us);
    if (info.alpha_mode != AC_ALPHA_OPAQUE) ScanAlpha(f, info, &alpha);
    if (ref_on) {
      const size_t got = ffmpeg.ReadFully(ref_frame.data(), frame_bytes);
      if (got != frame_bytes) {
        ref_problem = Fmt("ffmpeg produced only %zu frames, the core decoded more", hashes.size() - 1);
        ref_on = false;
      } else {
        std::string where;
        if (!SameAsReference(f, ref_frame.data(), &where)) {
          if (ref_bad_frames++ == 0) ref_first_bad = Fmt("frame %zu (pts %lld us), %s", hashes.size() - 1,
                                                          static_cast<long long>(f.pts_us), where.c_str());
        }
      }
    }
  }

  if (!decode_error.empty()) {
    r.Fail("decode", decode_error);
  } else {
    r.Ok("decode", Fmt("%zu frames from %zu packets, no errors%s", hashes.size(), n,
                       hashes.size() != n ? " (some packets produce no frame)" : ""));
  }

  // ---- alpha -----------------------------------------------------------------------------------------------------
  if (decode_error.empty() && info.alpha_mode != AC_ALPHA_OPAQUE && alpha.any) {
    if (alpha.min == alpha.max) {
      r.Warn("alpha", Fmt("the alpha is %d in every pixel of every frame: the clip has no transparency to speak of "
                          "(was the alpha lost on the way in?)",
                          alpha.min));
    } else {
      r.Ok("alpha", Fmt("values %d..%d over all frames", alpha.min, alpha.max));
    }
    if (alpha.transparent >= 1000) {
      const double dark = static_cast<double>(alpha.transparent_dark) / static_cast<double>(alpha.transparent);
      if (info.premultiplied && dark < 0.95) {
        r.Warn("alpha", Fmt("the sidecar says the colour is premultiplied, but under only %.0f%% of the transparent "
                            "pixels it is black: the flag looks wrong, which makes edges too bright",
                            dark * 100.0));
      } else if (!info.premultiplied && dark >= 0.95) {
        r.Warn("alpha", Fmt("the colour under %.0f%% of the transparent pixels is black but %s: when the video is "
                            "scaled up, bilinear filtering can blend that black into tight edges (a dark rim); "
                            "premultiplied colour (the default of `alphaclip encode`) does not darken them, and straight "
                            "colour that continues under the transparent pixels is the most accurate",
                            dark * 100.0,
                            have_meta ? "the sidecar says it is not premultiplied"
                                      : "there is no sidecar, so it is read as not premultiplied"));
      }
    }
  } else if (info.alpha_mode != AC_ALPHA_OPAQUE) {
    r.Skip("alpha", "the decode failed");
  }

  // ---- seek ------------------------------------------------------------------------------------------------------
  if (!decode_error.empty()) {
    r.Skip("seek", "the decode failed");
  } else {
    std::vector<size_t> seekable;
    for (size_t i = 0; i < n; ++i) {
      if (a.packets[i].seekable) seekable.push_back(i);
    }
    std::vector<size_t> targets;
    const size_t step = std::max<size_t>(1, (seekable.size() + 23) / 24);  // at most ~24 seek points
    for (size_t k = 0; k < seekable.size(); k += step) {
      const size_t s = seekable[k];
      const size_t next = k + step < seekable.size() ? seekable[k + step] : n;
      targets.push_back(s);
      if (s + 1 < next) targets.push_back(s + (next - s) / 2);  // the middle of the gap: pre-roll from the seek point
    }
    targets.push_back(n - 1);
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

    size_t checked = 0, bad = 0;
    std::string first_bad;
    auto check = [&](int64_t seek_pts, size_t packet) {
      const auto it = std::lower_bound(frame_pts.begin(), frame_pts.end(), a.packets[packet].pts_us);
      if (it == frame_pts.end()) return;  // a hidden-only packet at the very end: no frame to compare with
      const size_t expect = static_cast<size_t>(it - frame_pts.begin());
      ++checked;
      std::string what;
      ac_frame f;
      if (ac_seek(player.p, seek_pts) != AC_OK) {
        what = Fmt("ac_seek failed: %s", ac_last_error(player.p));
      } else if (ac_decode_next(player.p, &f) != AC_OK) {
        what = Fmt("no frame after the seek: %s", ac_last_error(player.p));
      } else if (f.pts_us != frame_pts[expect]) {
        what = Fmt("got the frame at %lld us, expected the one at %lld us", static_cast<long long>(f.pts_us),
                   static_cast<long long>(frame_pts[expect]));
      } else if (HashFrame(f) != hashes[expect]) {
        what = "the frame differs from the one sequential decoding produces";
      }
      if (!what.empty() && bad++ == 0) {
        first_bad = Fmt("seeking to %lld us (packet %zu): %s", static_cast<long long>(seek_pts), packet, what.c_str());
      }
    };
    for (size_t t : targets) check(a.packets[t].pts_us, t);
    check(a.packets[0].pts_us - 1000000, 0);              // before the start
    check(a.packets[n - 1].pts_us + 1000000, n - 1);      // past the end
    if (bad != 0) r.Fail("seek", Fmt("%zu of %zu seeks returned the wrong frame; the first: %s", bad, checked, first_bad.c_str()));
    else r.Ok("seek", Fmt("%zu seeks (seek points, the middle of the gaps, before the start, past the end) all return "
                          "the frame sequential decoding shows",
                          checked));
  }

  // ---- ffmpeg ----------------------------------------------------------------------------------------------------
  if (!ref_note.empty() && !ref_on && ref_problem.empty()) {
    if (o.ffmpeg_explicit) r.Fail("ffmpeg", ref_note);
    else r.Skip("ffmpeg", ref_note + " (give --ffmpeg <path> or set ALPHACLIP_FFMPEG to compare with an independent decode)");
  } else if (!decode_error.empty()) {
    ffmpeg.Terminate();
    r.Skip("ffmpeg", "the decode failed");
  } else if (!ref_problem.empty()) {
    r.Fail("ffmpeg", ref_problem);
    ffmpeg.Terminate();
  } else {
    uint8_t extra = 0;
    const size_t more = ffmpeg.ReadFully(&extra, 1);
    const int code = ffmpeg.Wait();
    if (ref_bad_frames != 0) {
      r.Fail("ffmpeg", Fmt("%zu of %zu frames differ from the reference decode; the first: %s", ref_bad_frames,
                           hashes.size(), ref_first_bad.c_str()));
    } else if (more != 0) {
      r.Fail("ffmpeg", Fmt("ffmpeg produced more frames than the core's %zu", hashes.size()));
    } else if (code != 0) {
      r.Fail("ffmpeg", Fmt("the reference decode failed (ffmpeg exited with code %d)", code));
    } else {
      r.Ok("ffmpeg", Fmt("all %zu frames are byte-identical to the reference decode (%s)", hashes.size(),
                         ffmpeg_version.c_str()));
    }
  }
  return r.Finish();
}

int CmdVerify(int argc, char** argv) {
  VerifyOptions o;
  o.clip_path = argv[2];
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--meta") == 0 && i + 1 < argc) o.meta_path = argv[++i];
    else if (std::strcmp(argv[i], "--no-meta") == 0) o.no_meta = true;
    else if (std::strcmp(argv[i], "--ffmpeg") == 0 && i + 1 < argc) {
      o.ffmpeg = argv[++i];
      o.ffmpeg_explicit = true;
    } else if (std::strcmp(argv[i], "--no-ffmpeg") == 0) o.no_ffmpeg = true;
    else return Usage();
  }
  if ((o.no_meta && !o.meta_path.empty()) || (o.no_ffmpeg && o.ffmpeg_explicit)) return Usage();
  return RunVerify(o);
}

}  // namespace cli
