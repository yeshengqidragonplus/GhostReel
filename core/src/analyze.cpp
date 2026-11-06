#include "analyze.h"

#include <algorithm>
#include <limits>

#include "common.h"
#include "demuxer.h"
#include "vp9_decoder.h"

namespace ac {
namespace {

std::string NotKeyframeText(const PacketFacts& f, bool stream_has_alpha) {
  if (!stream_has_alpha) return "the packet is not a keyframe";
  if (!f.color_key && !f.alpha_key) return "the packet is not a keyframe on either the colour or the alpha stream";
  if (!f.color_key) return "the packet is not a keyframe on the colour stream";
  return "the packet is not a keyframe on the alpha stream";
}

}  // namespace

ClipAnalysis AnalyzeWebm(const uint8_t* data, size_t size) {
  const StreamIndex idx = ParseWebm(data, size, /*collect_cues=*/true);

  ClipAnalysis a;
  a.codec_id = idx.codec_id;
  a.width = idx.width;
  a.height = idx.height;
  a.alpha_mode_flag = idx.alpha_mode_flag;
  a.default_frame_us = idx.default_frame_us;
  a.container_duration_us = idx.container_duration_us;
  a.cluster_count = idx.clusters.size();

  for (const PacketRef& p : idx.packets) {
    if (p.color_offset > size || p.color_size > size - p.color_offset) {
      throw Failure(AC_ERR_FORMAT, "a video frame extends past the end of the file");
    }
    if (!p.alpha.empty()) ++a.alpha_packets;
  }
  a.stream_has_alpha = idx.alpha_mode_flag && a.alpha_packets != 0;

  const bool vp9 = idx.codec_id == "V_VP9";
  a.packets.reserve(idx.packets.size());
  for (const PacketRef& p : idx.packets) {
    PacketFacts f;
    f.pts_us = p.pts_us;
    f.color_size = p.color_size;
    f.alpha_size = static_cast<uint32_t>(std::min<size_t>(p.alpha.size(), std::numeric_limits<uint32_t>::max()));
    if (vp9) {
      const Vp9Peek pc = PeekVp9(data + p.color_offset, p.color_size);
      f.color_key = pc.valid && pc.keyframe;
      if (!p.alpha.empty()) {
        const Vp9Peek pa = PeekVp9(p.alpha.data(), p.alpha.size());
        f.alpha_key = pa.valid && pa.keyframe;
      }
    }
    f.seekable = f.color_key && (!a.stream_has_alpha || f.alpha_key);
    a.packets.push_back(f);
  }

  // Which packets belong to which cluster (packets are in file order, so the ranges are contiguous).
  constexpr size_t kNone = std::numeric_limits<size_t>::max();
  std::vector<size_t> first(idx.clusters.size(), kNone);
  std::vector<size_t> last(idx.clusters.size(), 0);
  for (size_t i = 0; i < idx.packets.size(); ++i) {
    const size_t c = idx.packets[i].cluster;
    if (c >= first.size()) continue;
    if (first[c] == kNone) first[c] = i;
    last[c] = i;
  }

  for (const CueRef& c : idx.cues) {
    if (c.has_track && c.track != idx.video_track) continue;  // a cue for another track: not our business
    CueFact f;
    f.time_us = c.time_us;
    if (!c.has_time || !c.has_track || !c.has_cluster_position) {
      f.problem = "incomplete cue point (it needs CueTime, CueTrack and CueClusterPosition)";
    } else if (c.cluster_position > size) {
      f.problem = "CueClusterPosition lies outside the file";
    } else {
      const uint64_t abs = idx.segment_data_start + c.cluster_position;
      const auto it = std::lower_bound(idx.clusters.begin(), idx.clusters.end(), abs,
                                       [](const ClusterRef& r, uint64_t v) { return r.position < v; });
      if (it == idx.clusters.end() || it->position != abs) {
        f.problem = "CueClusterPosition " + std::to_string(c.cluster_position) + " is not the start of a cluster";
      } else {
        const size_t ci = static_cast<size_t>(it - idx.clusters.begin());
        int found = -1;
        if (first[ci] != kNone) {
          for (size_t i = first[ci]; i <= last[ci]; ++i) {
            if (idx.packets[i].cluster == ci && idx.packets[i].pts_us == c.time_us) {
              found = static_cast<int>(i);
              break;
            }
          }
        }
        if (found < 0) {
          f.problem = "no video packet with the cue's time in the cluster it points at";
        } else {
          f.packet = found;
          const PacketRef& p = idx.packets[static_cast<size_t>(found)];
          if (c.has_relative_position &&
              (c.relative_position > size || p.element_pos != it->data_start + c.relative_position)) {
            f.problem = "CueRelativePosition does not match the position of the packet";
          } else if (!a.packets[static_cast<size_t>(found)].seekable) {
            f.problem = NotKeyframeText(a.packets[static_cast<size_t>(found)], a.stream_has_alpha);
          }
        }
      }
    }
    a.cues.push_back(std::move(f));
  }
  return a;
}

}  // namespace ac
