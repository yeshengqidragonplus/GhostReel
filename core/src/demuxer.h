// Internal. WebM demuxing with libwebm's event-driven `webm_parser`.
//
// M0 finding: `mkvparser` cannot read BlockAdditions at all, so it cannot see the alpha
// stream; `webm_parser` can. The whole file is scanned once at open time and a packet
// index is built. Colour frame bodies are NOT copied (only their offset and size are
// kept); the alpha payload, which is tiny, is copied. Because the index covers every
// packet, seeking never needs the container's Cues.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ac {

struct PacketRef {
  int64_t pts_us = 0;
  uint64_t color_offset = 0;      // into the buffer given to ParseWebm
  uint32_t color_size = 0;
  std::vector<uint8_t> alpha;     // BlockAdditional (BlockAddID 1); empty when the block has none
};

struct StreamIndex {
  std::string codec_id;           // e.g. "V_VP9"
  int width = 0;
  int height = 0;
  bool alpha_mode_flag = false;   // TrackEntry/Video/AlphaMode == 1
  int64_t container_duration_us = 0;  // Info/Duration, 0 when absent
  int64_t default_frame_us = 0;       // TrackEntry/DefaultDuration, 0 when absent
  std::vector<PacketRef> packets;     // stream (decode) order
};

// Scans `data` and returns the index of the first video track.
// Throws ac::Failure: AC_ERR_FORMAT for malformed/truncated input, AC_ERR_UNSUPPORTED for
// laced video blocks, AC_ERR_OOM when a limit in common.h is exceeded.
StreamIndex ParseWebm(const uint8_t* data, size_t size);

}  // namespace ac
