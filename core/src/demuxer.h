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
  // Only filled when ParseWebm is asked to collect the Cues (authoring tools, M5):
  uint64_t element_pos = 0;       // file position of the SimpleBlock / BlockGroup element that holds the packet
  uint32_t cluster = 0;           // index into StreamIndex::clusters
};

// Only collected with `collect_cues`.
struct ClusterRef {
  uint64_t position = 0;          // file position of the Cluster element
  uint64_t data_start = 0;        // file position of its first child element (what CueRelativePosition counts from)
};

// One CueTrackPositions of one CuePoint (a CuePoint without any is recorded once with has_track == false).
struct CueRef {
  int64_t time_us = 0;
  uint64_t track = 0;
  uint64_t cluster_position = 0;  // as written: relative to the first byte after the Segment header
  uint64_t relative_position = 0; // as written: relative to the first child of the cluster
  bool has_time = false;
  bool has_track = false;
  bool has_cluster_position = false;
  bool has_relative_position = false;
};

struct StreamIndex {
  std::string codec_id;           // e.g. "V_VP9"
  int width = 0;
  int height = 0;
  bool alpha_mode_flag = false;   // TrackEntry/Video/AlphaMode == 1
  int64_t container_duration_us = 0;  // Info/Duration, 0 when absent
  int64_t default_frame_us = 0;       // TrackEntry/DefaultDuration, 0 when absent
  std::vector<PacketRef> packets;     // stream (decode) order
  // Only filled with `collect_cues`:
  uint64_t video_track = 0;           // track number of the video track
  uint64_t segment_data_start = 0;    // file position of the first byte after the Segment header
  std::vector<ClusterRef> clusters;   // file order
  std::vector<CueRef> cues;           // file order
};

// Scans `data` and returns the index of the first video track.
// With `collect_cues` the Cues, the clusters and the position of every packet are recorded as well (for `inspect` /
// `verify`; the player does not need them and does not ask).
// Throws ac::Failure: AC_ERR_FORMAT for malformed/truncated input, AC_ERR_UNSUPPORTED for
// laced video blocks, AC_ERR_OOM when a limit in common.h is exceeded.
StreamIndex ParseWebm(const uint8_t* data, size_t size, bool collect_cues = false);

}  // namespace ac
