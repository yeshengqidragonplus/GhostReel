// Internal. Container-level facts about a WebM clip, for the authoring tools (`alphaclip inspect` / `verify`, M5).
// Nothing here decodes a frame: it reads the container (packets, keyframe flags of the colour and the alpha stream,
// Cues) and checks every Cue against the packet it points at. The player does not use it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ac {

struct PacketFacts {
  int64_t pts_us = 0;
  uint32_t color_size = 0;
  uint32_t alpha_size = 0;    // 0: the block carries no alpha payload
  bool color_key = false;     // the colour stream has a keyframe here
  bool alpha_key = false;     // the alpha stream has a keyframe here (false when there is no alpha payload)
  bool seekable = false;      // decoding may start here: keyframe on every stream the file has
};

struct CueFact {
  int64_t time_us = 0;
  int packet = -1;            // index of the packet the cue points at, -1 when it points at none
  std::string problem;        // empty: the cue points at a packet where both streams have a keyframe
};

struct ClipAnalysis {
  std::string codec_id;
  int width = 0;
  int height = 0;
  bool alpha_mode_flag = false;     // TrackEntry/Video/AlphaMode == 1
  bool stream_has_alpha = false;    // AlphaMode flag and at least one packet with alpha payload
  size_t alpha_packets = 0;         // packets that carry an alpha payload
  int64_t default_frame_us = 0;     // TrackEntry/DefaultDuration, 0 when absent
  int64_t container_duration_us = 0;
  size_t cluster_count = 0;
  std::vector<PacketFacts> packets;
  std::vector<CueFact> cues;        // cues for the video track, file order; empty = the file has no usable Cues
};

// Throws ac::Failure like ParseWebm (AC_ERR_FORMAT / AC_ERR_UNSUPPORTED / AC_ERR_OOM). A codec other than V_VP9 is
// not an error here (keyframe flags are then all false): the caller reports it.
ClipAnalysis AnalyzeWebm(const uint8_t* data, size_t size);

}  // namespace ac
