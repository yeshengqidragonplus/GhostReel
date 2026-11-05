// Internal. A parsed clip plus synchronous decoding. The threaded player is built on top of this.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "demuxer.h"
#include "vp9_decoder.h"

namespace ac {

// One decoded frame with its own storage, so it outlives later decode calls.
struct FrameBuffer {
  int width = 0;
  int height = 0;
  int64_t pts_us = 0;
  bool has_alpha = false;
  std::vector<uint8_t> plane[4];  // Y, U, V, A; tightly packed
  int stride[4] = {0, 0, 0, 0};

  void Allocate(int w, int h, bool alpha);
};

struct ClipInfo {
  int width = 0;
  int height = 0;
  int64_t duration_us = 0;
  double fps = 0;
  bool has_alpha = false;
  size_t packet_count = 0;
};

class Clip {
 public:
  // `data` must outlive the Clip (it is not copied). Throws ac::Failure.
  static std::unique_ptr<Clip> Open(const uint8_t* data, size_t size);

  const ClipInfo& info() const { return info_; }

  // The next DecodeNext() returns the frame shown at `pts_us` (latest packet with
  // pts <= pts_us, or the first packet). Restarts decoding at the closest earlier
  // keyframe on both streams, with fresh decoders. Clears a previous decode error.
  void Seek(int64_t pts_us);

  // Decodes the next shown frame into *out. Returns false at the end of the stream.
  // Throws ac::Failure on a decode error; after that, call Seek() to recover.
  bool DecodeNext(FrameBuffer* out);

 private:
  Clip() = default;
  void ResetDecoders();

  const uint8_t* data_ = nullptr;
  StreamIndex index_;
  ClipInfo info_;
  std::vector<uint8_t> seekable_;  // 1 where decoding may start (keyframe on every stream)

  std::unique_ptr<Vp9Decoder> color_;
  std::unique_ptr<Vp9Decoder> alpha_;
  size_t cursor_ = 0;          // next packet to decode
  size_t discard_before_ = 0;  // frames of packets below this index are decoded but not returned
  bool failed_ = false;
};

}  // namespace ac
