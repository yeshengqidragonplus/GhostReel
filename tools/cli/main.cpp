// alphaclip: developer command-line tool for the core library (M1).
//
//   alphaclip info   <clip.webm>
//   alphaclip decode <clip.webm> --out <raw.yuv> [--seek-us N] [--frames N]
//
// `decode` writes every frame as planar Y, U, V (4:2:0, tightly packed) followed by the
// A plane when the clip has alpha, i.e. the same layout as ffmpeg's raw yuv420p / yuva420p,
// so the output can be compared byte for byte with an ffmpeg reference decode.
//
// Exit codes: 0 ok, 1 error (message on stderr), 2 usage.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "alphaclip.h"

namespace {

int Usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  alphaclip info   <clip.webm>\n"
               "  alphaclip decode <clip.webm> --out <raw.yuv> [--seek-us N] [--frames N]\n"
               "  alphaclip --version\n");
  return 2;
}

bool ParseInt64(const char* text, int64_t* out) {
  if (text == nullptr || *text == '\0') return false;
  errno = 0;
  char* end = nullptr;
  const long long v = std::strtoll(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') return false;
  *out = static_cast<int64_t>(v);
  return true;
}

int CmdInfo(const char* path) {
  ac_player* p = nullptr;
  if (ac_open_file(path, nullptr, &p) != AC_OK) {
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
    } else {
      return Usage();
    }
  }
  if (out_path == nullptr) return Usage();

  ac_player* p = nullptr;
  if (ac_open_file(path, nullptr, &p) != AC_OK) {
    std::fprintf(stderr, "error: %s\n", ac_last_error(nullptr));
    return 1;
  }
  int rc = 0;
  std::FILE* out = std::fopen(out_path, "wb");
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
  if (argc >= 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("alphaclip core %s\n", ac_version());
    return 0;
  }
  if (argc >= 3 && std::strcmp(argv[1], "info") == 0 && argc == 3) return CmdInfo(argv[2]);
  if (argc >= 3 && std::strcmp(argv[1], "decode") == 0) return CmdDecode(argc, argv);
  return Usage();
}
