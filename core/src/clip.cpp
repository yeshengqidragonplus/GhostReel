#include "clip.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

#include "common.h"

namespace ac {
namespace {

void CopyPlane(const uint8_t* src, int src_stride, int w, int h, std::vector<uint8_t>* dst) {
  uint8_t* d = dst->data();
  for (int y = 0; y < h; ++y) {
    std::memcpy(d + static_cast<size_t>(y) * static_cast<size_t>(w),
                src + static_cast<ptrdiff_t>(y) * src_stride, static_cast<size_t>(w));
  }
}

bool PlaneUsable(const vpx_image_t* img, int plane, int w) {
  return img->planes[plane] != nullptr && img->stride[plane] >= w;
}

std::string SizeText(int w, int h) { return std::to_string(w) + "x" + std::to_string(h); }

}  // namespace

void FrameBuffer::Allocate(int w, int h, bool alpha) {
  width = w;
  height = h;
  has_alpha = alpha;
  const int cw = (w + 1) / 2;
  const int ch = (h + 1) / 2;
  stride[0] = w;
  stride[1] = cw;
  stride[2] = cw;
  stride[3] = alpha ? w : 0;
  plane[0].resize(static_cast<size_t>(w) * static_cast<size_t>(h));
  plane[1].resize(static_cast<size_t>(cw) * static_cast<size_t>(ch));
  plane[2].resize(static_cast<size_t>(cw) * static_cast<size_t>(ch));
  plane[3].resize(alpha ? static_cast<size_t>(w) * static_cast<size_t>(h) : 0);
}

std::unique_ptr<Clip> Clip::Open(const uint8_t* data, size_t size) {
  std::unique_ptr<Clip> clip(new Clip());
  clip->data_ = data;
  clip->index_ = ParseWebm(data, size);
  const StreamIndex& idx = clip->index_;

  if (idx.codec_id != "V_VP9") {
    throw Failure(AC_ERR_UNSUPPORTED,
                  "unsupported video codec '" + idx.codec_id + "' (only V_VP9 is supported)");
  }
  if (idx.width <= 0 || idx.height <= 0) {
    throw Failure(AC_ERR_FORMAT, "the video track has no valid size");
  }
  if (idx.width > kMaxDimension || idx.height > kMaxDimension ||
      static_cast<int64_t>(idx.width) * idx.height > kMaxPixels) {
    throw Failure(AC_ERR_OOM, "video size " + SizeText(idx.width, idx.height) +
                                  " exceeds the supported maximum");
  }
  if (idx.packets.empty()) throw Failure(AC_ERR_FORMAT, "the video track has no frames");

  // Alpha: AlphaMode=1 in the track header plus a BlockAdditional (id 1) on every packet.
  size_t with_alpha = 0;
  for (const PacketRef& p : idx.packets) {
    if (p.color_offset > size || p.color_size > size - p.color_offset) {
      throw Failure(AC_ERR_FORMAT, "a video frame extends past the end of the file");
    }
    if (!p.alpha.empty()) ++with_alpha;
  }
  if (idx.alpha_mode_flag && with_alpha != 0 && with_alpha != idx.packets.size()) {
    throw Failure(AC_ERR_FORMAT, "only " + std::to_string(with_alpha) + " of " +
                                     std::to_string(idx.packets.size()) +
                                     " packets carry alpha data");
  }
  const bool has_alpha = idx.alpha_mode_flag && with_alpha != 0;

  // Keyframes: a seek may only start where BOTH streams have a keyframe. The size is
  // checked on every keyframe so that a corrupt header cannot make libvpx allocate a
  // different-sized frame buffer.
  const size_t n = idx.packets.size();
  clip->seekable_.assign(n, 0);
  for (size_t i = 0; i < n; ++i) {
    const PacketRef& p = idx.packets[i];
    const Vp9Peek pc = PeekVp9(data + p.color_offset, p.color_size);
    bool keyframe = pc.valid && pc.keyframe;
    if (keyframe && (pc.width != idx.width || pc.height != idx.height)) {
      throw Failure(AC_ERR_UNSUPPORTED, "packet " + std::to_string(i) + ": keyframe size " +
                                            SizeText(pc.width, pc.height) +
                                            " differs from the track size " +
                                            SizeText(idx.width, idx.height) +
                                            " (resolution changes are not supported)");
    }
    if (has_alpha) {
      const Vp9Peek pa = PeekVp9(p.alpha.data(), p.alpha.size());
      const bool alpha_key = pa.valid && pa.keyframe;
      if (alpha_key && (pa.width != idx.width || pa.height != idx.height)) {
        throw Failure(AC_ERR_UNSUPPORTED, "packet " + std::to_string(i) + ": alpha keyframe size " +
                                              SizeText(pa.width, pa.height) +
                                              " differs from the track size");
      }
      keyframe = keyframe && alpha_key;
    }
    clip->seekable_[i] = keyframe ? 1 : 0;
  }
  if (!clip->seekable_[0]) {
    throw Failure(AC_ERR_FORMAT, has_alpha
                                     ? "the first packet is not a keyframe on both the colour and the alpha stream"
                                     : "the first packet is not a keyframe");
  }

  ClipInfo& info = clip->info_;
  info.width = idx.width;
  info.height = idx.height;
  info.has_alpha = has_alpha;
  info.packet_count = n;
  const int64_t first = idx.packets.front().pts_us;
  const int64_t last = idx.packets.back().pts_us;
  int64_t frame_us = idx.default_frame_us;
  if (frame_us <= 0 && n > 1) frame_us = (last - first) / static_cast<int64_t>(n - 1);
  info.fps = frame_us > 0 ? 1e6 / static_cast<double>(frame_us) : 0.0;
  info.duration_us =
      idx.container_duration_us > 0 ? idx.container_duration_us : last + std::max<int64_t>(frame_us, 0);

  clip->ResetDecoders();
  return clip;
}

void Clip::ResetDecoders() {
  color_.reset(new Vp9Decoder());
  if (info_.has_alpha) alpha_.reset(new Vp9Decoder());
  else alpha_.reset();
}

void Clip::Seek(int64_t pts_us) {
  const std::vector<PacketRef>& pk = index_.packets;
  // j = last packet with pts <= pts_us (the first packet when the target is earlier).
  const auto it = std::upper_bound(pk.begin(), pk.end(), pts_us,
                                   [](int64_t t, const PacketRef& p) { return t < p.pts_us; });
  const size_t j = it == pk.begin() ? 0 : static_cast<size_t>(it - pk.begin()) - 1;
  size_t k = j;
  while (k > 0 && !seekable_[k]) --k;  // seekable_[0] is guaranteed by Open()
  ResetDecoders();
  cursor_ = k;
  discard_before_ = j;
  failed_ = false;
}

bool Clip::DecodeNext(FrameBuffer* out) {
  if (failed_) {
    throw Failure(AC_ERR_STATE, "a previous decode error left the decoder unusable; seek to recover");
  }
  size_t i = 0;
  try {
    while (cursor_ < index_.packets.size()) {
      i = cursor_++;
      const PacketRef& p = index_.packets[i];
      const vpx_image_t* c = color_->Decode(data_ + p.color_offset, p.color_size);
      const vpx_image_t* a = nullptr;
      if (info_.has_alpha) a = alpha_->Decode(p.alpha.data(), p.alpha.size());
      if (i < discard_before_) continue;  // pre-roll after a seek: decoded to build references only

      if (c == nullptr) {
        if (a != nullptr) {
          throw Failure(AC_ERR_DECODE, "the alpha stream produced a frame where the colour stream did not");
        }
        continue;  // hidden frame: nothing to show for this packet
      }
      if (info_.has_alpha && a == nullptr) {
        throw Failure(AC_ERR_DECODE, "the colour stream produced a frame where the alpha stream did not");
      }

      const int w = info_.width;
      const int h = info_.height;
      const int cw = (w + 1) / 2;
      if (static_cast<int>(c->d_w) != w || static_cast<int>(c->d_h) != h ||
          (a != nullptr && (static_cast<int>(a->d_w) != w || static_cast<int>(a->d_h) != h))) {
        throw Failure(AC_ERR_UNSUPPORTED,
                      "the decoded frame size " + SizeText(static_cast<int>(c->d_w), static_cast<int>(c->d_h)) +
                          " differs from the track size " + SizeText(w, h) +
                          " (resolution changes are not supported)");
      }
      if (!PlaneUsable(c, VPX_PLANE_Y, w) || !PlaneUsable(c, VPX_PLANE_U, cw) ||
          !PlaneUsable(c, VPX_PLANE_V, cw) || (a != nullptr && !PlaneUsable(a, VPX_PLANE_Y, w))) {
        throw Failure(AC_ERR_DECODE, "the decoder returned an invalid image");
      }

      out->Allocate(w, h, a != nullptr);
      CopyPlane(c->planes[VPX_PLANE_Y], c->stride[VPX_PLANE_Y], w, h, &out->plane[0]);
      CopyPlane(c->planes[VPX_PLANE_U], c->stride[VPX_PLANE_U], cw, (h + 1) / 2, &out->plane[1]);
      CopyPlane(c->planes[VPX_PLANE_V], c->stride[VPX_PLANE_V], cw, (h + 1) / 2, &out->plane[2]);
      if (a != nullptr) CopyPlane(a->planes[VPX_PLANE_Y], a->stride[VPX_PLANE_Y], w, h, &out->plane[3]);
      out->pts_us = p.pts_us;
      return true;
    }
    return false;
  } catch (const Failure& f) {
    failed_ = true;
    throw Failure(f.status(), "packet " + std::to_string(i) + ": " + f.what());
  } catch (...) {
    failed_ = true;
    throw;
  }
}

}  // namespace ac
