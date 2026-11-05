// M0 spike (throwaway, NOT the future core library).
//
// Question this answers (design doc R2 / section 5):
//   Can we recover VP9 alpha from a WebM file using only libwebm + libvpx,
//   i.e. without FFmpeg?
//
// How: libwebm's `webm_parser` (the event-driven parser; `mkvparser` does not
// expose BlockAdditions at all) yields, per frame, the colour VP9 packet plus
// the BlockAdditional payload (BlockAddID 1). We run TWO libvpx VP9 decoder
// instances: one for colour, one for the alpha stream (alpha lives in the Y
// plane of the second stream).
//
// Usage: alpha_probe <in.webm> [--dump out.yuva] [--verbose] [--start N]
//   --dump   writes, per frame: Y, U, V (I420, tightly packed) then A (w*h bytes).
//   --start  begin decoding at packet N with FRESH decoders (what a seek to a
//            keyframe does at the decoder level). N must be a keyframe.
//   --corrupt-alpha SEED  after demux, flip random bytes inside the alpha payloads
//            only (colour stays intact): "alpha stream is bad, colour is fine".
// Exit codes: 0 ok (alpha for every frame, or an opaque file that decodes),
//             1 clean failure, 2 usage/IO, 3 exception caught from the parser.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <random>
#include <string>
#include <vector>

#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <webm/callback.h>
#include <webm/file_reader.h>
#include <webm/status.h>
#include <webm/webm_parser.h>

using webm::Action;
using webm::Status;

namespace {

// A corrupt size field must not make us allocate gigabytes. The real core will
// derive its limit from the remaining file size and the declared dimensions.
constexpr uint64_t kMaxFrameBytes = 64ull << 20;

struct Packet {
  double pts_ms = 0;
  std::vector<uint8_t> color;
  std::vector<uint8_t> alpha;  // empty => no BlockAdditional with id 1
  bool simple_block = false;
};

// Collects every video packet (+ its alpha additional data) from the file.
class Collector : public webm::Callback {
 public:
  uint64_t timecode_scale_ns = 1000000;
  uint64_t video_track = 0;
  std::string codec_id;
  uint64_t width = 0, height = 0;
  uint64_t alpha_mode = 0;  // TrackEntry/Video/AlphaMode
  size_t cue_points = 0;
  size_t blocks_with_lacing = 0;
  size_t additions_other_ids = 0;
  std::vector<Packet> packets;

  Status OnInfo(const webm::ElementMetadata&, const webm::Info& info) override {
    if (info.timecode_scale.is_present()) timecode_scale_ns = info.timecode_scale.value();
    return Status(Status::kOkCompleted);
  }

  Status OnTrackEntry(const webm::ElementMetadata&, const webm::TrackEntry& t) override {
    if (video_track == 0 && t.track_type.is_present() &&
        t.track_type.value() == webm::TrackType::kVideo) {
      video_track = t.track_number.value();
      codec_id = t.codec_id.value();
      if (t.video.is_present()) {
        width = t.video.value().pixel_width.value();
        height = t.video.value().pixel_height.value();
        alpha_mode = t.video.value().alpha_mode.value();
      }
    }
    return Status(Status::kOkCompleted);
  }

  Status OnCuePoint(const webm::ElementMetadata&, const webm::CuePoint&) override {
    ++cue_points;
    return Status(Status::kOkCompleted);
  }

  Status OnClusterBegin(const webm::ElementMetadata&, const webm::Cluster& c,
                        Action* action) override {
    cluster_timecode_ = c.timecode.value();
    *action = Action::kRead;
    return Status(Status::kOkCompleted);
  }

  Status OnSimpleBlockBegin(const webm::ElementMetadata&, const webm::SimpleBlock& sb,
                            Action* action) override {
    BeginBlock(sb.track_number, sb.timecode);
    *action = Action::kRead;
    return Status(Status::kOkCompleted);
  }

  Status OnBlockBegin(const webm::ElementMetadata&, const webm::Block& b,
                      Action* action) override {
    BeginBlock(b.track_number, b.timecode);
    *action = Action::kRead;
    return Status(Status::kOkCompleted);
  }

  Status OnFrame(const webm::FrameMetadata& m, webm::Reader* reader,
                 uint64_t* bytes_remaining) override {
    if (*bytes_remaining == 0) return Status(Status::kOkCompleted);
    uint64_t n = 0;
    if (cur_track_ != video_track) {
      Status s = reader->Skip(*bytes_remaining, &n);
      *bytes_remaining -= n;
      return s;
    }
    if (*bytes_remaining == m.size) {  // first call for this frame
      if (m.size > kMaxFrameBytes) return Status(Status::kNotEnoughMemory);
      cur_color_.assign(static_cast<size_t>(m.size), 0);
      if (++frames_in_block_ > 1) ++blocks_with_lacing;
    }
    const size_t off = static_cast<size_t>(m.size - *bytes_remaining);
    Status s = reader->Read(static_cast<size_t>(*bytes_remaining), cur_color_.data() + off, &n);
    *bytes_remaining -= n;
    return s;
  }

  Status OnSimpleBlockEnd(const webm::ElementMetadata&, const webm::SimpleBlock&) override {
    if (cur_track_ == video_track) {
      Packet p;
      p.pts_ms = cur_pts_ms_;
      p.color = std::move(cur_color_);
      p.simple_block = true;
      packets.push_back(std::move(p));
    }
    cur_color_.clear();
    return Status(Status::kOkCompleted);
  }

  Status OnBlockGroupEnd(const webm::ElementMetadata&, const webm::BlockGroup& bg) override {
    if (cur_track_ == video_track) {
      Packet p;
      p.pts_ms = cur_pts_ms_;
      p.color = std::move(cur_color_);
      if (bg.additions.is_present()) {
        for (const auto& more : bg.additions.value().block_mores) {
          if (more.value().id.value() == 1) {
            p.alpha = more.value().data.value();
          } else {
            ++additions_other_ids;
          }
        }
      }
      packets.push_back(std::move(p));
    }
    cur_color_.clear();
    return Status(Status::kOkCompleted);
  }

 private:
  void BeginBlock(uint64_t track, int16_t rel_timecode) {
    cur_track_ = track;
    frames_in_block_ = 0;
    cur_pts_ms_ = static_cast<double>(static_cast<int64_t>(cluster_timecode_) + rel_timecode) *
                  static_cast<double>(timecode_scale_ns) / 1e6;
  }

  uint64_t cluster_timecode_ = 0;
  uint64_t cur_track_ = 0;
  size_t frames_in_block_ = 0;
  double cur_pts_ms_ = 0;
  std::vector<uint8_t> cur_color_;
};

double NowMs() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

bool InitDecoder(vpx_codec_ctx_t* ctx) {
  vpx_codec_dec_cfg_t cfg{};
  cfg.threads = 1;  // single-thread: we want an honest per-frame cost
  return vpx_codec_dec_init(ctx, vpx_codec_vp9_dx(), &cfg, 0) == VPX_CODEC_OK;
}

// Decode one packet; returns the frame (valid until the next decode on ctx) or null.
const vpx_image_t* DecodeOne(vpx_codec_ctx_t* ctx, const std::vector<uint8_t>& data,
                             const char* what, size_t index) {
  if (vpx_codec_decode(ctx, data.data(), static_cast<unsigned>(data.size()), nullptr, 0) !=
      VPX_CODEC_OK) {
    const char* detail = vpx_codec_error_detail(ctx);
    std::fprintf(stderr, "  frame %zu: %s decode error: %s (%s)\n", index, what,
                 vpx_codec_error(ctx), detail ? detail : "");
    return nullptr;
  }
  vpx_codec_iter_t iter = nullptr;
  return vpx_codec_get_frame(ctx, &iter);
}

// -1 = unknown (not parseable), 0 = inter frame, 1 = keyframe
int PeekKeyframe(const std::vector<uint8_t>& data) {
  vpx_codec_stream_info_t si{};
  si.sz = sizeof(si);
  if (vpx_codec_peek_stream_info(vpx_codec_vp9_dx(), data.data(),
                                 static_cast<unsigned>(data.size()), &si) != VPX_CODEC_OK)
    return -1;
  return si.is_kf ? 1 : 0;
}

void AppendPlane(std::vector<uint8_t>* out, const uint8_t* p, int stride, int w, int h) {
  for (int y = 0; y < h; ++y) out->insert(out->end(), p + y * stride, p + y * stride + w);
}

std::string JoinIdx(const std::vector<size_t>& v) {
  std::string s;
  for (size_t i = 0; i < v.size() && i < 16; ++i) s += (i ? "," : "") + std::to_string(v[i]);
  if (v.size() > 16) s += ",...";
  return s.empty() ? "-" : s;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <in.webm> [--dump out.yuva] [--verbose] [--start N]\n", argv[0]);
    return 2;
  }
  const char* path = argv[1];
  const char* dump_path = nullptr;
  bool verbose = false;
  size_t start_index = 0;
  bool corrupt_alpha = false;
  uint32_t corrupt_seed = 0;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) dump_path = argv[++i];
    else if (!std::strcmp(argv[i], "--verbose")) verbose = true;
    else if (!std::strcmp(argv[i], "--corrupt-alpha") && i + 1 < argc) {
      corrupt_alpha = true;
      corrupt_seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    }
    else if (!std::strcmp(argv[i], "--start") && i + 1 < argc)
      start_index = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
  }

  FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", path);
    return 2;
  }

  // ---- 1. demux ----
  Collector col;
  webm::FileReader reader(f);  // takes ownership of f
  webm::WebmParser parser;
  Status st(Status::kOkCompleted);
  const double t_demux0 = NowMs();
  try {
    st = parser.Feed(&col, &reader);
  } catch (const std::exception& e) {
    std::printf("RESULT: FAIL (demux threw: %s)\n", e.what());
    return 3;
  } catch (...) {
    std::printf("RESULT: FAIL (demux threw a non-std exception)\n");
    return 3;
  }
  const double t_demux = NowMs() - t_demux0;
  const bool parse_ok = st.completed_ok() || st.code == Status::kEndOfFile;
  std::printf("== demux ==\n");
  std::printf("parser status        : %d (%s)\n", st.code, parse_ok ? "ok" : "ERROR");
  std::printf("video track / codec  : %llu / %s\n", (unsigned long long)col.video_track,
              col.codec_id.c_str());
  std::printf("size (TrackEntry)    : %llux%llu\n", (unsigned long long)col.width,
              (unsigned long long)col.height);
  std::printf("AlphaMode element    : %llu\n", (unsigned long long)col.alpha_mode);
  std::printf("Cues (CuePoints)     : %zu\n", col.cue_points);
  std::printf("video packets        : %zu\n", col.packets.size());
  size_t with_alpha = 0, in_simple = 0;
  for (const auto& p : col.packets) {
    with_alpha += !p.alpha.empty();
    in_simple += p.simple_block;
  }
  std::printf("packets w/ alpha data: %zu (SimpleBlock: %zu)\n", with_alpha, in_simple);
  std::printf("other BlockAddIDs    : %zu, laced blocks: %zu\n", col.additions_other_ids,
              col.blocks_with_lacing);
  std::printf("demux time           : %.2f ms\n", t_demux);
  if (!parse_ok || col.packets.empty()) {
    std::printf("RESULT: FAIL (demux)\n");
    return 1;
  }
  if (col.codec_id != "V_VP9") {
    std::printf("RESULT: FAIL (codec is not V_VP9)\n");
    return 1;
  }
  if (start_index >= col.packets.size()) {
    std::printf("RESULT: FAIL (--start %zu >= %zu packets)\n", start_index, col.packets.size());
    return 1;
  }

  if (corrupt_alpha) {
    std::mt19937 rng(corrupt_seed);
    for (auto& p : col.packets) {
      if (p.alpha.empty()) continue;
      const unsigned flips = 1 + rng() % 8;
      for (unsigned k = 0; k < flips; ++k) p.alpha[rng() % p.alpha.size()] ^= uint8_t(1 + rng() % 255);
    }
  }

  // ---- 2. decode: two independent VP9 decoders ----
  vpx_codec_ctx_t dc{}, da{};
  if (!InitDecoder(&dc) || !InitDecoder(&da)) {
    std::fprintf(stderr, "vpx_codec_dec_init failed\n");
    return 1;
  }
  const size_t n_range = col.packets.size() - start_index;
  size_t alpha_expected = 0;
  for (size_t i = start_index; i < col.packets.size(); ++i) alpha_expected += !col.packets[i].alpha.empty();

  std::vector<uint8_t> dump;
  std::vector<size_t> kf_color, kf_alpha;
  size_t color_frames = 0, alpha_frames = 0, mismatches = 0;
  double ms_color = 0, ms_alpha = 0;
  int a_min = 255, a_max = 0;
  uint64_t a_zero = 0, a_full = 0, a_part = 0;

  std::printf("\n== decode (packets %zu..%zu) ==\n", start_index, col.packets.size() - 1);
  for (size_t i = start_index; i < col.packets.size(); ++i) {
    const Packet& p = col.packets[i];
    const int ckf = PeekKeyframe(p.color);
    const int akf = p.alpha.empty() ? -2 : PeekKeyframe(p.alpha);
    if (ckf == 1) kf_color.push_back(i);
    if (akf == 1) kf_alpha.push_back(i);
    if (i == start_index && ckf != 1)
      std::printf("WARNING: --start packet %zu is not a colour keyframe\n", i);

    double t0 = NowMs();
    const vpx_image_t* ci = DecodeOne(&dc, p.color, "color", i);
    double t1 = NowMs();
    ms_color += t1 - t0;
    if (!ci) {
      ++mismatches;
      continue;
    }
    ++color_frames;

    const vpx_image_t* ai = nullptr;
    if (!p.alpha.empty()) {
      t0 = NowMs();
      ai = DecodeOne(&da, p.alpha, "alpha", i);
      ms_alpha += NowMs() - t0;
      if (ai) ++alpha_frames;
      else ++mismatches;
    }
    if (verbose || i < start_index + 3) {
      std::printf("frame %3zu pts=%8.2fms color=%6zuB alpha=%6zuB kf(c/a)=%d/%d  color:%dx%d fmt=0x%x cs=%d range=%d",
                  i, p.pts_ms, p.color.size(), p.alpha.size(), ckf, akf, ci->d_w, ci->d_h, ci->fmt,
                  (int)ci->cs, (int)ci->range);
      if (ai) std::printf("  alpha:%dx%d fmt=0x%x range=%d", ai->d_w, ai->d_h, ai->fmt, (int)ai->range);
      std::printf("\n");
    }
    if (ci->fmt != VPX_IMG_FMT_I420) {
      std::printf("RESULT: FAIL (unexpected colour format 0x%x)\n", ci->fmt);
      return 1;
    }
    if (ai && (ai->d_w != ci->d_w || ai->d_h != ci->d_h)) {
      std::printf("frame %zu: alpha size %dx%d != colour size %dx%d\n", i, ai->d_w, ai->d_h,
                  ci->d_w, ci->d_h);
      ++mismatches;
      ai = nullptr;
    }

    if (ai) {  // alpha statistics (Y plane of the second stream)
      for (unsigned y = 0; y < ai->d_h; ++y) {
        const uint8_t* row = ai->planes[0] + y * ai->stride[0];
        for (unsigned x = 0; x < ai->d_w; ++x) {
          const int v = row[x];
          a_min = std::min(a_min, v);
          a_max = std::max(a_max, v);
          (v == 0 ? a_zero : v == 255 ? a_full : a_part)++;
        }
      }
    }

    if (dump_path) {
      const int w = ci->d_w, h = ci->d_h, cw = (w + 1) / 2, chh = (h + 1) / 2;
      AppendPlane(&dump, ci->planes[0], ci->stride[0], w, h);
      AppendPlane(&dump, ci->planes[1], ci->stride[1], cw, chh);
      AppendPlane(&dump, ci->planes[2], ci->stride[2], cw, chh);
      if (ai) AppendPlane(&dump, ai->planes[0], ai->stride[0], w, h);
      else dump.insert(dump.end(), (size_t)w * h, 255);
    }
  }

  std::printf("\n== summary ==\n");
  std::printf("colour frames decoded : %zu / %zu\n", color_frames, n_range);
  std::printf("alpha frames decoded  : %zu / %zu\n", alpha_frames, alpha_expected);
  std::printf("mismatches/errors     : %zu\n", mismatches);
  std::printf("avg decode ms/frame   : colour %.3f, alpha %.3f (single thread)\n",
              color_frames ? ms_color / color_frames : 0.0,
              alpha_frames ? ms_alpha / alpha_frames : 0.0);
  std::printf("keyframes colour      : %s\n", JoinIdx(kf_color).c_str());
  std::printf("keyframes alpha       : %s\n", JoinIdx(kf_alpha).c_str());
  const bool kf_aligned = alpha_expected == 0 || kf_color == kf_alpha;
  if (alpha_expected) std::printf("keyframes aligned     : %s\n", kf_aligned ? "yes" : "NO (seek would break alpha)");
  if (alpha_frames) {
    const double total = double(a_zero + a_full + a_part);
    std::printf("alpha value range     : min=%d max=%d\n", a_min, a_max);
    std::printf("alpha pixels          : 0:%.2f%%  255:%.2f%%  partial:%.2f%%\n",
                100.0 * a_zero / total, 100.0 * a_full / total, 100.0 * a_part / total);
  }
  if (dump_path) {
    FILE* o = std::fopen(dump_path, "wb");
    if (o) {
      std::fwrite(dump.data(), 1, dump.size(), o);
      std::fclose(o);
      std::printf("dumped %zu bytes to %s\n", dump.size(), dump_path);
    }
  }
  vpx_codec_destroy(&dc);
  vpx_codec_destroy(&da);

  if (alpha_expected == 0) {
    const bool ok = color_frames == n_range && mismatches == 0;
    std::printf("RESULT: %s\n", ok ? "OPAQUE (no alpha data in file; all colour frames decoded)" : "FAIL");
    return ok ? 0 : 1;
  }
  const bool ok = color_frames == n_range && alpha_frames == alpha_expected &&
                  alpha_expected == n_range && mismatches == 0;
  std::printf("RESULT: %s\n", ok ? "PASS (alpha recovered for every frame)" : "FAIL");
  return ok ? 0 : 1;
}
