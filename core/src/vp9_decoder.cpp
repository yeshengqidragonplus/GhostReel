#include "vp9_decoder.h"

#include <climits>
#include <cstring>
#include <string>

#include <vpx/vp8dx.h>

#include "common.h"

namespace ac {

Vp9Peek PeekVp9(const uint8_t* data, size_t size) {
  Vp9Peek out;
  if (data == nullptr || size == 0 || size > UINT_MAX) return out;
  vpx_codec_stream_info_t si;
  std::memset(&si, 0, sizeof(si));
  si.sz = sizeof(si);
  if (vpx_codec_peek_stream_info(vpx_codec_vp9_dx(), data, static_cast<unsigned>(size), &si) !=
      VPX_CODEC_OK) {
    return out;
  }
  out.valid = true;
  out.keyframe = si.is_kf != 0;
  out.width = static_cast<int>(si.w);
  out.height = static_cast<int>(si.h);
  return out;
}

Vp9Decoder::Vp9Decoder() {
  std::memset(&ctx_, 0, sizeof(ctx_));
  vpx_codec_dec_cfg_t cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.threads = 1;  // the core runs its own decode thread; libvpx threading is a later knob
  if (vpx_codec_dec_init(&ctx_, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) {
    const char* detail = vpx_codec_error(&ctx_);
    throw Failure(AC_ERR_DECODE, std::string("cannot initialise the VP9 decoder: ") +
                                     (detail ? detail : "unknown error"));
  }
}

Vp9Decoder::~Vp9Decoder() { vpx_codec_destroy(&ctx_); }

const vpx_image_t* Vp9Decoder::Decode(const uint8_t* data, size_t size) {
  if (size == 0 || size > UINT_MAX) {
    throw Failure(AC_ERR_DECODE, "empty or oversized video packet");
  }
  if (vpx_codec_decode(&ctx_, data, static_cast<unsigned>(size), nullptr, 0) != VPX_CODEC_OK) {
    const char* detail = vpx_codec_error_detail(&ctx_);
    throw Failure(AC_ERR_DECODE, std::string("libvpx: ") + vpx_codec_error(&ctx_) +
                                     (detail ? std::string(" (") + detail + ")" : std::string()));
  }
  vpx_codec_iter_t iter = nullptr;
  const vpx_image_t* img = vpx_codec_get_frame(&ctx_, &iter);
  if (img == nullptr) return nullptr;
  if (vpx_codec_get_frame(&ctx_, &iter) != nullptr) {
    throw Failure(AC_ERR_UNSUPPORTED, "a video packet produced more than one frame");
  }
  if (img->fmt != VPX_IMG_FMT_I420 || img->bit_depth != 8) {
    throw Failure(AC_ERR_UNSUPPORTED, "only 8-bit 4:2:0 video is supported");
  }
  return img;
}

}  // namespace ac
