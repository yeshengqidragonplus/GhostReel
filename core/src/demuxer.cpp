#include "demuxer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include <webm/callback.h>
#include <webm/reader.h>
#include <webm/status.h>
#include <webm/webm_parser.h>

#include "common.h"

namespace ac {
namespace {

// Reads from a caller-owned buffer; never copies more than the parser asks for.
class MemoryReader final : public webm::Reader {
 public:
  MemoryReader(const uint8_t* data, uint64_t size) : data_(data), size_(size) {}

  webm::Status Read(std::size_t num_to_read, std::uint8_t* buffer,
                    std::uint64_t* num_actually_read) override {
    const uint64_t n = std::min<uint64_t>(num_to_read, size_ - pos_);
    *num_actually_read = n;
    if (n == 0) return webm::Status(webm::Status::kEndOfFile);
    std::memcpy(buffer, data_ + pos_, static_cast<size_t>(n));
    pos_ += n;
    return webm::Status(n == num_to_read ? webm::Status::kOkCompleted : webm::Status::kOkPartial);
  }

  webm::Status Skip(std::uint64_t num_to_skip, std::uint64_t* num_actually_skipped) override {
    const uint64_t n = std::min<uint64_t>(num_to_skip, size_ - pos_);
    *num_actually_skipped = n;
    if (n == 0) return webm::Status(webm::Status::kEndOfFile);
    pos_ += n;
    return webm::Status(n == num_to_skip ? webm::Status::kOkCompleted : webm::Status::kOkPartial);
  }

  std::uint64_t Position() const override { return pos_; }

 private:
  const uint8_t* data_;
  uint64_t size_;
  uint64_t pos_ = 0;
};

int64_t TicksToUs(int64_t ticks, uint64_t scale_ns) {
  const int64_t scale = static_cast<int64_t>(scale_ns);  // validated: 0 < scale <= INT64_MAX
  if (ticks > std::numeric_limits<int64_t>::max() / scale ||
      ticks < std::numeric_limits<int64_t>::min() / scale) {
    throw Failure(AC_ERR_FORMAT, "timestamp is out of range");
  }
  return ticks * scale / 1000;
}

// Collects the index of the first video track.
class Collector final : public webm::Callback {
 public:
  Collector(uint64_t file_size, bool collect_cues) : file_size_(file_size), collect_cues_(collect_cues) {}

  webm::Status OnElementBegin(const webm::ElementMetadata& m, webm::Action* action) override {
    // No element may claim to extend past the end of the file. This bounds every
    // allocation the parser makes on our behalf (e.g. an alpha payload vector) by the
    // file size, so a corrupt size field cannot request gigabytes.
    if (m.size != webm::kUnknownElementSize && m.position != webm::kUnknownElementPosition &&
        m.header_size != webm::kUnknownHeaderSize) {
      if (m.position > file_size_ || m.header_size > file_size_ - m.position ||
          m.size > file_size_ - m.position - m.header_size) {
        throw Failure(AC_ERR_FORMAT, "a WebM element extends past the end of the file");
      }
    }
    *action = webm::Action::kRead;
    return webm::Status(webm::Status::kOkCompleted);
  }

  webm::Status OnInfo(const webm::ElementMetadata&, const webm::Info& info) override {
    if (info.timecode_scale.is_present()) {
      const uint64_t v = info.timecode_scale.value();
      if (v == 0 || v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        throw Failure(AC_ERR_FORMAT, "invalid TimecodeScale");
      }
      timecode_scale_ns_ = v;
    }
    if (info.duration.is_present()) duration_ticks_ = info.duration.value();
    return webm::Status(webm::Status::kOkCompleted);
  }

  webm::Status OnTrackEntry(const webm::ElementMetadata&, const webm::TrackEntry& t) override {
    if (video_track_ == 0 && t.track_type.is_present() &&
        t.track_type.value() == webm::TrackType::kVideo && t.track_number.is_present() &&
        t.track_number.value() != 0) {
      video_track_ = t.track_number.value();
      codec_id_ = t.codec_id.value();
      if (t.default_duration.is_present()) default_duration_ns_ = t.default_duration.value();
      if (t.video.is_present()) {
        const uint64_t w = t.video.value().pixel_width.value();
        const uint64_t h = t.video.value().pixel_height.value();
        // Out-of-range values are rejected later (Clip::Open) with a clear message.
        width_ = w > static_cast<uint64_t>(std::numeric_limits<int>::max()) ? -1 : static_cast<int>(w);
        height_ = h > static_cast<uint64_t>(std::numeric_limits<int>::max()) ? -1 : static_cast<int>(h);
        alpha_mode_flag_ = t.video.value().alpha_mode.value() == 1;
      }
    }
    return webm::Status(webm::Status::kOkCompleted);
  }

  webm::Status OnSegmentBegin(const webm::ElementMetadata& m, webm::Action* action) override {
    // Cue positions count from the first byte after the Segment header. (OnElementBegin has already checked that
    // position + header_size lies inside the file.)
    if (m.position != webm::kUnknownElementPosition && m.header_size != webm::kUnknownHeaderSize) {
      segment_data_start_ = m.position + m.header_size;
    }
    *action = webm::Action::kRead;
    return webm::Status(webm::Status::kOkCompleted);
  }

  webm::Status OnClusterBegin(const webm::ElementMetadata& m, const webm::Cluster& c,
                              webm::Action* action) override {
    const uint64_t tc = c.timecode.value();
    if (tc > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      throw Failure(AC_ERR_FORMAT, "cluster timecode is out of range");
    }
    cluster_timecode_ = static_cast<int64_t>(tc);
    if (collect_cues_) {
      if (clusters_.size() >= kMaxPackets) {
        throw Failure(AC_ERR_OOM, "too many clusters (limit " + std::to_string(kMaxPackets) + ")");
      }
      ClusterRef ref;
      if (m.position != webm::kUnknownElementPosition && m.header_size != webm::kUnknownHeaderSize) {
        ref.position = m.position;
        ref.data_start = m.position + m.header_size;
      }
      clusters_.push_back(ref);
    }
    *action = webm::Action::kRead;
    return webm::Status(webm::Status::kOkCompleted);
  }

  webm::Status OnBlockGroupBegin(const webm::ElementMetadata& m, webm::Action* action) override {
    group_pos_ = m.position == webm::kUnknownElementPosition ? 0 : m.position;
    *action = webm::Action::kRead;
    return webm::Status(webm::Status::kOkCompleted);
  }

  webm::Status OnSimpleBlockBegin(const webm::ElementMetadata& m, const webm::SimpleBlock& sb,
                                  webm::Action* action) override {
    BeginBlock(sb.track_number, sb.timecode, m.position == webm::kUnknownElementPosition ? 0 : m.position);
    *action = webm::Action::kRead;
    return webm::Status(webm::Status::kOkCompleted);
  }

  webm::Status OnBlockBegin(const webm::ElementMetadata&, const webm::Block& b,
                            webm::Action* action) override {
    BeginBlock(b.track_number, b.timecode, group_pos_);  // a Cue points at the BlockGroup, not the Block inside it
    *action = webm::Action::kRead;
    return webm::Status(webm::Status::kOkCompleted);
  }

  webm::Status OnCuePoint(const webm::ElementMetadata&, const webm::CuePoint& cp) override {
    if (!collect_cues_) return webm::Status(webm::Status::kOkCompleted);
    CueRef base;
    base.has_time = cp.time.is_present();
    if (base.has_time) {
      const uint64_t t = cp.time.value();
      if (t > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        throw Failure(AC_ERR_FORMAT, "cue time is out of range");
      }
      base.time_us = TicksToUs(static_cast<int64_t>(t), timecode_scale_ns_);
    }
    auto push = [this](const CueRef& c) {
      if (cues_.size() >= kMaxPackets) {
        throw Failure(AC_ERR_OOM, "too many cue points (limit " + std::to_string(kMaxPackets) + ")");
      }
      cues_.push_back(c);
    };
    if (cp.cue_track_positions.empty()) push(base);
    for (const auto& e : cp.cue_track_positions) {
      CueRef c = base;
      const webm::CueTrackPositions& pos = e.value();
      c.has_track = pos.track.is_present();
      if (c.has_track) c.track = pos.track.value();
      c.has_cluster_position = pos.cluster_position.is_present();
      if (c.has_cluster_position) c.cluster_position = pos.cluster_position.value();
      c.has_relative_position = pos.relative_position.is_present();
      if (c.has_relative_position) c.relative_position = pos.relative_position.value();
      push(c);
    }
    return webm::Status(webm::Status::kOkCompleted);
  }

  webm::Status OnFrame(const webm::FrameMetadata& m, webm::Reader* reader,
                       uint64_t* bytes_remaining) override {
    if (*bytes_remaining == 0) return webm::Status(webm::Status::kOkCompleted);
    if (cur_is_video_ && *bytes_remaining == m.size) {  // first call for this frame
      if (++frames_in_block_ > 1) {
        throw Failure(AC_ERR_UNSUPPORTED, "laced video blocks are not supported");
      }
      if (m.size > std::numeric_limits<uint32_t>::max()) {
        throw Failure(AC_ERR_FORMAT, "video frame is larger than 4 GiB");
      }
      cur_offset_ = reader->Position();
      cur_size_ = static_cast<uint32_t>(m.size);
    }
    // The frame body is never copied here; the decoder reads it from the source buffer.
    uint64_t skipped = 0;
    const webm::Status s = reader->Skip(*bytes_remaining, &skipped);
    *bytes_remaining -= skipped;
    return s;
  }

  webm::Status OnSimpleBlockEnd(const webm::ElementMetadata&, const webm::SimpleBlock&) override {
    PushPacket(nullptr);
    return webm::Status(webm::Status::kOkCompleted);
  }

  webm::Status OnBlockGroupEnd(const webm::ElementMetadata&, const webm::BlockGroup& bg) override {
    PushPacket(&bg);
    return webm::Status(webm::Status::kOkCompleted);
  }

  StreamIndex Finish() {
    if (video_track_ == 0) throw Failure(AC_ERR_FORMAT, "the file has no video track");
    StreamIndex s;
    s.codec_id = codec_id_;
    s.width = width_;
    s.height = height_;
    s.alpha_mode_flag = alpha_mode_flag_;
    s.default_frame_us = static_cast<int64_t>(
        std::min<uint64_t>(default_duration_ns_ / 1000, std::numeric_limits<int64_t>::max()));
    if (std::isfinite(duration_ticks_) && duration_ticks_ > 0) {
      const double us = duration_ticks_ * static_cast<double>(timecode_scale_ns_) / 1000.0;
      s.container_duration_us = us < 1e15 ? static_cast<int64_t>(us) : 0;
    }
    for (size_t i = 1; i < packets_.size(); ++i) {
      if (packets_[i].pts_us < packets_[i - 1].pts_us) {
        throw Failure(AC_ERR_FORMAT, "video timestamps are not monotonic (packet " +
                                         std::to_string(i) + ")");
      }
    }
    s.packets = std::move(packets_);
    if (collect_cues_) {
      s.video_track = video_track_;
      s.segment_data_start = segment_data_start_;
      s.clusters = std::move(clusters_);
      s.cues = std::move(cues_);
    }
    return s;
  }

 private:
  void BeginBlock(uint64_t track, int16_t rel_timecode, uint64_t element_pos) {
    cur_is_video_ = video_track_ != 0 && track == video_track_;
    frames_in_block_ = 0;
    cur_offset_ = 0;
    cur_size_ = 0;
    cur_element_pos_ = element_pos;
    if (cur_is_video_) {
      const int64_t ticks = cluster_timecode_ + rel_timecode;  // cluster_timecode_ <= INT64_MAX
      cur_pts_us_ = TicksToUs(ticks, timecode_scale_ns_);
    }
  }

  void PushPacket(const webm::BlockGroup* bg) {
    if (cur_is_video_ && cur_size_ > 0) {
      if (packets_.size() >= kMaxPackets) {
        throw Failure(AC_ERR_OOM, "too many video packets (limit " + std::to_string(kMaxPackets) + ")");
      }
      PacketRef p;
      p.pts_us = cur_pts_us_;
      p.color_offset = cur_offset_;
      p.color_size = cur_size_;
      if (collect_cues_) {
        p.element_pos = cur_element_pos_;
        p.cluster = clusters_.empty() ? 0 : static_cast<uint32_t>(clusters_.size() - 1);
      }
      if (bg != nullptr && bg->additions.is_present()) {
        for (const auto& more : bg->additions.value().block_mores) {
          if (more.value().id.value() == 1) p.alpha = more.value().data.value();
        }
      }
      packets_.push_back(std::move(p));
    }
    cur_is_video_ = false;
    cur_size_ = 0;
  }

  const uint64_t file_size_;
  const bool collect_cues_;
  uint64_t segment_data_start_ = 0;
  uint64_t group_pos_ = 0;
  uint64_t cur_element_pos_ = 0;
  std::vector<ClusterRef> clusters_;
  std::vector<CueRef> cues_;
  uint64_t timecode_scale_ns_ = 1000000;
  double duration_ticks_ = 0;
  uint64_t video_track_ = 0;
  std::string codec_id_;
  int width_ = 0;
  int height_ = 0;
  bool alpha_mode_flag_ = false;
  uint64_t default_duration_ns_ = 0;
  int64_t cluster_timecode_ = 0;

  bool cur_is_video_ = false;
  size_t frames_in_block_ = 0;
  uint64_t cur_offset_ = 0;
  uint32_t cur_size_ = 0;
  int64_t cur_pts_us_ = 0;
  std::vector<PacketRef> packets_;
};

}  // namespace

StreamIndex ParseWebm(const uint8_t* data, size_t size, bool collect_cues) {
  Collector collector(size, collect_cues);
  MemoryReader reader(data, size);
  webm::WebmParser parser;
  const webm::Status status = parser.Feed(&collector, &reader);
  if (!status.completed_ok()) {
    throw Failure(AC_ERR_FORMAT, "malformed or truncated WebM file (parser status " +
                                     std::to_string(status.code) + ")");
  }
  return collector.Finish();
}

}  // namespace ac
