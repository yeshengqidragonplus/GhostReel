// alphaclip: command-line tool for the core library. M1: info / decode / play (developer tools); M4: --meta; M5: the
// authoring commands encode / inspect / verify (inspect.cpp, verify.cpp, encode.cpp; see their headers).
//
//   alphaclip encode <input> --out <clip.webm> [...]       footage -> clip.webm + clip.acmeta.json, then verify
//   alphaclip inspect <clip.webm>                          what is in the file and how the core reads it
//   alphaclip verify  <clip.webm>                          is the clip fit to ship (exit code 0 / 1)
//
//   alphaclip info   <clip.webm> [--meta <clip.acmeta.json>]
//   alphaclip decode <clip.webm> --out <raw.yuv> [--seek-us N] [--frames N] [--meta <json>]
//
// --meta passes the sidecar metadata file to the core (see docs/m4-notes.md); without it the clip is "auto".
//
// `decode` writes every frame as planar Y, U, V (4:2:0, tightly packed) followed by the
// A plane when the clip has alpha, i.e. the same layout as ffmpeg's raw yuv420p / yuva420p,
// so the output can be compared byte for byte with an ffmpeg reference decode.
//
//   alphaclip play   <clip.webm> --out <raw.yuv> [--clock-step-us N] [--depth N]
//                    [--realtime-speed N] [--wait-end] [--wait-queue-full] [--loop]
//                    [--seek-us N] [--mid-seek-after N --mid-seek-us N]
//                    [--frames N] [--timeout-ms N]
//
// `play` drives the threaded player (ac_start / ac_acquire_frame / ac_release_frame) with a
// simulated clock that advances by --clock-step-us per iteration (or, with --realtime-speed N,
// with the wall clock times N), writes each frame it is handed in the same raw layout as
// `decode`, and prints one "frame pts_us=N" line per frame plus the player statistics.
//   --wait-end         first wait until the decode thread has decoded the whole clip (needs
//                      --depth >= the packet count): the run becomes deterministic
//   --wait-queue-full  first wait until the queue is full (deterministic for the first
//                      `depth` frames; works with --loop)
//   --loop             ac_set_loop(1)
//   --seek-us T        ac_seek(T) right after the start; the clock starts at T
//   --mid-seek-after N --mid-seek-us T
//                      after N frames were shown: ac_seek(T), restart the clock at T, and print
//                      a "seek pts_us=T" line
//
// Exit codes: 0 ok, 1 error (message on stderr), 2 usage.
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "alphaclip.h"
#include "commands.h"
#include "util.h"

namespace cli {

int Usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  alphaclip encode  <input> --out <clip.webm> [--mode auto|native_alpha|packed_right|packed_bottom|opaque]\n"
               "                    [--straight] [--fps N] [--crf N] [--gop N] [--matrix bt601|bt709] [--range limited|full]\n"
               "                    [--loop] [--cpu-used N] [--threads N] [--ffmpeg <file or folder>] [--force]\n"
               "                    [--no-verify] [--quiet] [--verbose]\n"
               "  alphaclip inspect <clip.webm> [--meta <clip.acmeta.json> | --no-meta]\n"
               "  alphaclip verify  <clip.webm> [--meta <clip.acmeta.json> | --no-meta] [--ffmpeg <file or folder> | --no-ffmpeg]\n"
               "  alphaclip info    <clip.webm> [--meta <clip.acmeta.json>]\n"
               "  alphaclip decode  <clip.webm> --out <raw.yuv> [--seek-us N] [--frames N] [--meta <json>]\n"
               "  alphaclip play    <clip.webm> --out <raw.yuv> [--meta <json>] [--clock-step-us N] [--depth N]\n"
               "                    [--realtime-speed N] [--wait-end] [--wait-queue-full] [--loop]\n"
               "                    [--seek-us N] [--mid-seek-after N --mid-seek-us N]\n"
               "                    [--frames N] [--timeout-ms N]\n"
               "  alphaclip --version\n");
  return 2;
}

}  // namespace cli

namespace {

using cli::ParseInt64;

int Usage() { return cli::Usage(); }

// --meta <file>: the sidecar metadata handed to ac_open_file (NULL when the option is not given).
std::string g_meta_text;
const char* g_meta = nullptr;

bool LoadMeta(const char* path) {
  std::FILE* f = cli::OpenFile(path, "rb");
  if (f == nullptr) {
    std::fprintf(stderr, "error: cannot open the metadata file '%s'\n", path);
    return false;
  }
  char buf[4096];
  size_t n;
  g_meta_text.clear();
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && g_meta_text.size() < (1u << 20)) {
    g_meta_text.append(buf, n);
  }
  std::fclose(f);
  g_meta = g_meta_text.c_str();  // an embedded NUL ends the text, as it would for any C caller
  return true;
}

const char* AlphaModeName(int mode) {
  switch (mode) {
    case AC_ALPHA_OPAQUE: return "opaque";
    case AC_ALPHA_PACKED_RIGHT: return "packed_right";
    case AC_ALPHA_PACKED_BOTTOM: return "packed_bottom";
    case AC_ALPHA_NATIVE: return "native_alpha";
    default: return "unknown";
  }
}

int CmdInfo(int argc, char** argv) {
  const char* path = argv[2];
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--meta") == 0 && i + 1 < argc) {
      if (!LoadMeta(argv[++i])) return 1;
    } else {
      return Usage();
    }
  }
  ac_player* p = nullptr;
  if (ac_open_file(path, g_meta, &p) != AC_OK) {
    std::fprintf(stderr, "error: %s\n", ac_last_error(nullptr));
    return 1;
  }
  ac_info info;
  if (ac_get_info(p, &info) != AC_OK) {
    std::fprintf(stderr, "error: %s\n", ac_last_error(p));
    ac_close(p);
    return 1;
  }
  std::printf("file: %s\n", path);
  std::printf("size: %dx%d\n", info.width, info.height);
  std::printf("alpha: %s\n", info.has_alpha_plane ? "yes" : "no");
  std::printf("packets: %lld\n", static_cast<long long>(info.packet_count));
  std::printf("duration_us: %lld\n", static_cast<long long>(info.duration_us));
  std::printf("fps: %.3f\n", static_cast<double>(info.fps));
  std::printf("loop_period_us: %lld\n", static_cast<long long>(info.loop_period_us));
  std::printf("alpha_mode: %s\n", AlphaModeName(info.alpha_mode));
  std::printf("content: %dx%d\n", info.content_w, info.content_h);
  std::printf("alpha_origin: %d,%d\n", info.alpha_x, info.alpha_y);
  std::printf("premultiplied: %s\n", info.premultiplied ? "yes" : "no");
  std::printf("color_range: %s\n", info.color_range == AC_RANGE_LIMITED ? "limited"
                                   : info.color_range == AC_RANGE_FULL ? "full" : "unspecified");
  std::printf("matrix: %s\n", info.matrix == AC_MATRIX_BT601 ? "bt601"
                              : info.matrix == AC_MATRIX_BT709 ? "bt709" : "unspecified");
  std::printf("loop_hint: %s\n", info.loop_hint ? "yes" : "no");
  ac_close(p);
  return 0;
}

bool WritePlane(std::FILE* f, const uint8_t* data, int stride, int w, int h) {
  for (int y = 0; y < h; ++y) {
    if (std::fwrite(data + static_cast<ptrdiff_t>(y) * stride, 1, static_cast<size_t>(w), f) !=
        static_cast<size_t>(w)) {
      return false;
    }
  }
  return true;
}

bool WriteFrame(std::FILE* f, const ac_frame& fr) {
  const int cw = (fr.width + 1) / 2;
  const int ch = (fr.height + 1) / 2;
  bool ok = WritePlane(f, fr.plane[0], fr.stride[0], fr.width, fr.height) &&
            WritePlane(f, fr.plane[1], fr.stride[1], cw, ch) &&
            WritePlane(f, fr.plane[2], fr.stride[2], cw, ch);
  if (ok && fr.plane[3] != nullptr) ok = WritePlane(f, fr.plane[3], fr.stride[3], fr.width, fr.height);
  return ok;
}

int CmdPlay(int argc, char** argv) {
  const char* path = argv[2];
  const char* out_path = nullptr;
  int64_t step_us = 1000;
  int64_t depth = 4;
  int64_t max_frames = -1;
  int64_t timeout_ms = 30000;
  int64_t realtime_speed = 0;  // 0: simulated clock; N >= 1: wall clock x N
  bool wait_end = false;
  bool wait_queue_full = false;
  bool loop = false;
  bool do_seek = false;
  int64_t seek_us = 0;
  int64_t mid_seek_after = -1;  // seek once, after this many frames have been shown
  int64_t mid_seek_us = 0;
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      out_path = argv[++i];
    } else if (std::strcmp(argv[i], "--clock-step-us") == 0 && i + 1 < argc) {
      if (!ParseInt64(argv[++i], &step_us) || step_us <= 0) return Usage();
    } else if (std::strcmp(argv[i], "--depth") == 0 && i + 1 < argc) {
      if (!ParseInt64(argv[++i], &depth) || depth < 1 || depth > 64) return Usage();
    } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      if (!ParseInt64(argv[++i], &max_frames) || max_frames < 0) return Usage();
    } else if (std::strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
      if (!ParseInt64(argv[++i], &timeout_ms) || timeout_ms <= 0) return Usage();
    } else if (std::strcmp(argv[i], "--realtime-speed") == 0 && i + 1 < argc) {
      if (!ParseInt64(argv[++i], &realtime_speed) || realtime_speed < 1 || realtime_speed > 1000) {
        return Usage();
      }
    } else if (std::strcmp(argv[i], "--meta") == 0 && i + 1 < argc) {
      if (!LoadMeta(argv[++i])) return 1;
    } else if (std::strcmp(argv[i], "--wait-end") == 0) {
      wait_end = true;
    } else if (std::strcmp(argv[i], "--wait-queue-full") == 0) {
      wait_queue_full = true;
    } else if (std::strcmp(argv[i], "--loop") == 0) {
      loop = true;
    } else if (std::strcmp(argv[i], "--seek-us") == 0 && i + 1 < argc) {
      if (!ParseInt64(argv[++i], &seek_us)) return Usage();
      do_seek = true;
    } else if (std::strcmp(argv[i], "--mid-seek-after") == 0 && i + 1 < argc) {
      if (!ParseInt64(argv[++i], &mid_seek_after) || mid_seek_after < 0) return Usage();
    } else if (std::strcmp(argv[i], "--mid-seek-us") == 0 && i + 1 < argc) {
      if (!ParseInt64(argv[++i], &mid_seek_us)) return Usage();
    } else {
      return Usage();
    }
  }
  if (out_path == nullptr) return Usage();
  if (loop && wait_end) {
    std::fprintf(stderr, "error: --wait-end never finishes with --loop; use --wait-queue-full\n");
    return 2;
  }

  ac_player* p = nullptr;
  if (ac_open_file(path, g_meta, &p) != AC_OK) {
    std::fprintf(stderr, "error: %s\n", ac_last_error(nullptr));
    return 1;
  }
  ac_info info;
  ac_get_info(p, &info);
  if (wait_end && depth < info.packet_count) {
    std::fprintf(stderr, "error: --wait-end needs --depth >= the packet count (%lld)\n",
                 static_cast<long long>(info.packet_count));
    ac_close(p);
    return 2;
  }
  std::FILE* out = cli::OpenFile(out_path, "wb");
  if (out == nullptr) {
    std::fprintf(stderr, "error: cannot create '%s'\n", out_path);
    ac_close(p);
    return 1;
  }

  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  int rc = 0;
  if (ac_set_queue_depth(p, static_cast<int32_t>(depth)) != AC_OK || ac_set_loop(p, loop ? 1 : 0) != AC_OK ||
      ac_start(p) != AC_OK) {
    std::fprintf(stderr, "error: %s\n", ac_last_error(p));
    rc = 1;
  }
  if (rc == 0 && do_seek && ac_seek(p, seek_us) != AC_OK) {
    std::fprintf(stderr, "error: %s\n", ac_last_error(p));
    rc = 1;
  }
  if (rc == 0 && (wait_end || wait_queue_full)) {
    ac_stats st;
    bool ready = false;
    do {
      ac_get_stats(p, &st);
      ready = wait_end ? st.reached_end != 0 : (st.reached_end != 0 || st.frames_queued >= depth);
      if (ready) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (Clock::now() < deadline);
    if (!ready) {
      std::fprintf(stderr, "error: timed out waiting for the decode thread\n");
      rc = 1;
    }
  }

  // The caller's clock: simulated (+step per iteration) or the wall clock x speed. It starts at
  // the seek target, and a seek restarts it at the new target, as an engine would do.
  int64_t clock_base = do_seek ? seek_us : 0;
  int64_t clock_us = clock_base;
  auto clock_epoch = Clock::now();
  int64_t frames = 0;
  bool mid_seek_done = false;
  while (rc == 0 && (max_frames < 0 || frames < max_frames)) {
    if (realtime_speed > 0) {
      clock_us = clock_base +
                 std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - clock_epoch).count() *
                     realtime_speed;
    }
    if (Clock::now() >= deadline) {
      std::fprintf(stderr, "error: timed out after %lld frames\n", static_cast<long long>(frames));
      rc = 1;
      break;
    }
    ac_frame f;
    const ac_status s = ac_acquire_frame(p, clock_us, &f);
    if (s == AC_END) break;
    if (s == AC_OK) {
      std::printf("frame pts_us=%lld\n", static_cast<long long>(f.pts_us));
      const bool ok = WriteFrame(out, f);
      ac_release_frame(p, &f);
      if (!ok) {
        std::fprintf(stderr, "error: cannot write to '%s'\n", out_path);
        rc = 1;
        break;
      }
      ++frames;
      if (mid_seek_after >= 0 && !mid_seek_done && frames >= mid_seek_after) {
        mid_seek_done = true;
        std::printf("seek pts_us=%lld\n", static_cast<long long>(mid_seek_us));
        if (ac_seek(p, mid_seek_us) != AC_OK) {
          std::fprintf(stderr, "error: %s\n", ac_last_error(p));
          rc = 1;
          break;
        }
        clock_base = mid_seek_us;
        clock_us = mid_seek_us;
        clock_epoch = Clock::now();
        continue;  // the clock restarts at the target; do not advance it this iteration
      }
    } else if (s == AC_AGAIN) {
      // No sleep: Windows sleeps for 1-15 ms at least, which would make a free-running
      // run take far longer than the clip. Yielding keeps the decode thread running.
      if (!wait_end) std::this_thread::yield();
    } else {
      std::fprintf(stderr, "error after %lld frames: %s\n", static_cast<long long>(frames),
                   ac_last_error(p));
      rc = 1;
      break;
    }
    if (realtime_speed == 0) clock_us += step_us;
  }
  if (std::fclose(out) != 0 && rc == 0) {
    std::fprintf(stderr, "error: cannot finish writing '%s'\n", out_path);
    rc = 1;
  }
  if (rc == 0) {
    ac_stats st;
    ac_get_stats(p, &st);
    std::printf("frames: %lld\n", static_cast<long long>(frames));
    std::printf("decoded: %lld\n", static_cast<long long>(st.frames_decoded));
    std::printf("acquired: %lld\n", static_cast<long long>(st.frames_acquired));
    std::printf("dropped: %lld\n", static_cast<long long>(st.frames_dropped));
    std::printf("decode_time_us: %lld\n", static_cast<long long>(st.decode_time_us));
  }
  ac_close(p);
  return rc;
}

int CmdDecode(int argc, char** argv) {
  const char* path = argv[2];
  const char* out_path = nullptr;
  int64_t seek_us = 0;
  bool do_seek = false;
  int64_t max_frames = -1;
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      out_path = argv[++i];
    } else if (std::strcmp(argv[i], "--seek-us") == 0 && i + 1 < argc) {
      if (!ParseInt64(argv[++i], &seek_us)) return Usage();
      do_seek = true;
    } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      if (!ParseInt64(argv[++i], &max_frames) || max_frames < 0) return Usage();
    } else if (std::strcmp(argv[i], "--meta") == 0 && i + 1 < argc) {
      if (!LoadMeta(argv[++i])) return 1;
    } else {
      return Usage();
    }
  }
  if (out_path == nullptr) return Usage();

  ac_player* p = nullptr;
  if (ac_open_file(path, g_meta, &p) != AC_OK) {
    std::fprintf(stderr, "error: %s\n", ac_last_error(nullptr));
    return 1;
  }
  int rc = 0;
  std::FILE* out = cli::OpenFile(out_path, "wb");
  if (out == nullptr) {
    std::fprintf(stderr, "error: cannot create '%s'\n", out_path);
    ac_close(p);
    return 1;
  }
  if (do_seek && ac_seek(p, seek_us) != AC_OK) {
    std::fprintf(stderr, "error: %s\n", ac_last_error(p));
    rc = 1;
  }
  int64_t frames = 0;
  int64_t first_pts = -1;
  int64_t last_pts = -1;
  while (rc == 0 && (max_frames < 0 || frames < max_frames)) {
    ac_frame f;
    const ac_status s = ac_decode_next(p, &f);
    if (s == AC_END) break;
    if (s != AC_OK) {
      std::fprintf(stderr, "error after %lld frames: %s\n", static_cast<long long>(frames),
                   ac_last_error(p));
      rc = 1;
      break;
    }
    const int cw = (f.width + 1) / 2;
    const int ch = (f.height + 1) / 2;
    bool ok = WritePlane(out, f.plane[0], f.stride[0], f.width, f.height) &&
              WritePlane(out, f.plane[1], f.stride[1], cw, ch) &&
              WritePlane(out, f.plane[2], f.stride[2], cw, ch);
    if (ok && f.plane[3] != nullptr) ok = WritePlane(out, f.plane[3], f.stride[3], f.width, f.height);
    if (!ok) {
      std::fprintf(stderr, "error: cannot write to '%s'\n", out_path);
      rc = 1;
      break;
    }
    if (frames == 0) first_pts = f.pts_us;
    last_pts = f.pts_us;
    ++frames;
  }
  if (std::fclose(out) != 0 && rc == 0) {
    std::fprintf(stderr, "error: cannot finish writing '%s'\n", out_path);
    rc = 1;
  }
  if (rc == 0) {
    std::printf("frames: %lld\n", static_cast<long long>(frames));
    std::printf("first_pts_us: %lld\n", static_cast<long long>(first_pts));
    std::printf("last_pts_us: %lld\n", static_cast<long long>(last_pts));
  }
  ac_close(p);
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  // Arguments as UTF-8 on every platform (a path with non-ASCII characters is fine on Windows too).
  std::vector<std::string> args = cli::Utf8Args(argc, argv);
  std::vector<char*> ptrs;
  for (std::string& a : args) ptrs.push_back(&a[0]);
  ptrs.push_back(nullptr);
  argc = static_cast<int>(args.size());
  argv = ptrs.data();

  if (argc >= 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("alphaclip core %s\n", ac_version());
    return 0;
  }
  if (argc >= 3 && std::strcmp(argv[1], "info") == 0) return CmdInfo(argc, argv);
  if (argc >= 3 && std::strcmp(argv[1], "decode") == 0) return CmdDecode(argc, argv);
  if (argc >= 3 && std::strcmp(argv[1], "play") == 0) return CmdPlay(argc, argv);
  if (argc >= 3 && std::strcmp(argv[1], "inspect") == 0) return cli::CmdInspect(argc, argv);
  if (argc >= 3 && std::strcmp(argv[1], "verify") == 0) return cli::CmdVerify(argc, argv);
  if (argc >= 3 && std::strcmp(argv[1], "encode") == 0) return cli::CmdEncode(argc, argv);
  return Usage();
}
