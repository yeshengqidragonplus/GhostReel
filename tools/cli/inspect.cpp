// `alphaclip inspect <clip.webm> [--meta <clip.acmeta.json> | --no-meta]` (design doc TR-2).
//
// Prints what is in the file and how the core reads it, one "key: value" line each (easy to parse):
//   file, file_size, codec, video_size, frame_rate, duration_us, packets, clusters, keyframes,
//   seek_granularity_frames, cues, alpha_stream, sidecar, metadata_schema, playable, and for a playable clip
//   alpha_mode, content_size, alpha_origin, premultiplied, color_range, matrix, loop_hint.
// Nothing is decoded. The sidecar defaults to `<clip>.acmeta.json` next to the clip when that exists.
// Exit code: 0 when the file could be read as WebM (even when the core refuses to play it: see `playable`), 1 when not.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "alphaclip.h"
#include "analyze.h"
#include "commands.h"
#include "common.h"
#include "meta.h"
#include "util.h"

namespace cli {
namespace {

const char* ModeName(int mode) {
  switch (mode) {
    case AC_ALPHA_OPAQUE: return "opaque";
    case AC_ALPHA_PACKED_RIGHT: return "packed_right";
    case AC_ALPHA_PACKED_BOTTOM: return "packed_bottom";
    case AC_ALPHA_NATIVE: return "native_alpha";
    default: return "unknown";
  }
}

}  // namespace

int CmdInspect(int argc, char** argv) {
  const std::string path = argv[2];
  std::string meta_path;
  bool no_meta = false;
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--meta") == 0 && i + 1 < argc) meta_path = argv[++i];
    else if (std::strcmp(argv[i], "--no-meta") == 0) no_meta = true;
    else return Usage();
  }
  if (no_meta && !meta_path.empty()) return Usage();

  std::vector<uint8_t> data;
  std::string error;
  if (!ReadFile(path, &data, &error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }

  // The sidecar.
  std::string meta_text;
  std::string meta_desc = "none (auto mode)";
  if (no_meta) {
    meta_desc = "none (--no-meta: auto mode)";
  } else if (!meta_path.empty()) {
    if (!ReadTextFile(meta_path, &meta_text, &error)) {
      std::fprintf(stderr, "error: %s\n", error.c_str());
      return 1;
    }
    meta_desc = meta_path + " (--meta)";
  } else {
    const std::string candidate = SidecarPathFor(path);
    if (FileExists(candidate)) {
      if (!ReadTextFile(candidate, &meta_text, &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
      }
      meta_desc = candidate + " (next to the clip)";
    }
  }

  ac::ClipAnalysis a;
  try {
    a = ac::AnalyzeWebm(data.data(), data.size());
  } catch (const ac::Failure& f) {
    std::fprintf(stderr, "error: %s\n", f.what());
    return 1;
  }

  const size_t n = a.packets.size();
  std::printf("file: %s\n", path.c_str());
  std::printf("file_size: %zu\n", data.size());
  std::printf("codec: %s\n", a.codec_id.c_str());
  std::printf("video_size: %dx%d\n", a.width, a.height);

  double fps = 0;
  const char* fps_source = "unknown";
  if (a.default_frame_us > 0) {
    fps = 1e6 / static_cast<double>(a.default_frame_us);
    fps_source = "DefaultDuration";
  } else if (n > 1 && a.packets.back().pts_us > a.packets.front().pts_us) {
    fps = 1e6 * static_cast<double>(n - 1) / static_cast<double>(a.packets.back().pts_us - a.packets.front().pts_us);
    fps_source = "measured";
  }
  std::printf("frame_rate: %.3f (%s)\n", fps, fps_source);
  std::printf("duration_us: %lld\n", static_cast<long long>(a.container_duration_us));
  std::printf("packets: %zu\n", n);
  std::printf("clusters: %zu\n", a.cluster_count);

  size_t color_keys = 0;
  size_t alpha_keys = 0;
  size_t seekable = 0;
  std::string seekable_list;
  size_t longest_gap = 0;
  size_t last_seekable = 0;
  bool have_seekable = false;
  for (size_t i = 0; i < n; ++i) {
    const ac::PacketFacts& f = a.packets[i];
    if (f.color_key) ++color_keys;
    if (f.alpha_key) ++alpha_keys;
    if (!f.seekable) continue;
    if (seekable < 12) seekable_list += (seekable == 0 ? "" : ",") + std::to_string(i);
    ++seekable;
    if (have_seekable) longest_gap = std::max(longest_gap, i - last_seekable);
    last_seekable = i;
    have_seekable = true;
  }
  if (have_seekable) longest_gap = std::max(longest_gap, n - last_seekable);
  if (seekable > 12) seekable_list += ",...";
  std::printf("keyframes: colour=%zu alpha=%zu seekable=%zu\n", color_keys, alpha_keys, seekable);
  std::printf("seekable_at_packets: %s\n", seekable_list.empty() ? "none" : seekable_list.c_str());
  std::printf("seek_granularity_frames: %zu\n", longest_gap);

  if (a.cues.empty()) {
    std::printf("cues: none\n");
  } else {
    size_t bad = 0;
    std::string first_problem;
    for (const ac::CueFact& c : a.cues) {
      if (c.problem.empty()) continue;
      if (bad++ == 0) first_problem = c.problem;
    }
    if (bad == 0) {
      std::printf("cues: %zu (all point at a packet where every stream has a keyframe)\n", a.cues.size());
    } else {
      std::printf("cues: %zu (%zu invalid; first: %s)\n", a.cues.size(), bad, first_problem.c_str());
    }
  }

  if (a.stream_has_alpha) {
    std::printf("alpha_stream: yes (AlphaMode=1, alpha data in %zu of %zu packets)\n", a.alpha_packets, n);
  } else if (a.alpha_mode_flag) {
    std::printf("alpha_stream: no (the AlphaMode flag is set but no packet carries alpha data)\n");
  } else if (a.alpha_packets != 0) {
    std::printf("alpha_stream: no (%zu packets carry alpha data but the AlphaMode flag is not set; the core ignores them)\n",
                a.alpha_packets);
  } else {
    std::printf("alpha_stream: no\n");
  }

  std::printf("sidecar: %s\n", meta_desc.c_str());
  if (!meta_text.empty()) {
    try {
      const ac::Meta m = ac::ParseMeta(meta_text.c_str());
      std::printf("metadata_schema: %d\n", m.schema);
    } catch (const ac::Failure&) {
      std::printf("metadata_schema: unreadable\n");
    }
  } else {
    std::printf("metadata_schema: none\n");
  }

  // How the core (and so every binding) reads the clip.
  ac_player* p = nullptr;
  const ac_status st = ac_open_memory(data.data(), data.size(), meta_text.empty() ? nullptr : meta_text.c_str(), &p);
  if (st != AC_OK) {
    std::printf("playable: no (%s)\n", ac_last_error(nullptr));
    return 0;
  }
  ac_info info;
  std::memset(&info, 0, sizeof(info));
  if (ac_get_info(p, &info) != AC_OK) {
    std::printf("playable: no (%s)\n", ac_last_error(p));
    ac_close(p);
    return 0;
  }
  std::printf("playable: yes\n");
  std::printf("alpha_mode: %s\n", ModeName(info.alpha_mode));
  std::printf("content_size: %dx%d\n", info.content_w, info.content_h);
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

}  // namespace cli
