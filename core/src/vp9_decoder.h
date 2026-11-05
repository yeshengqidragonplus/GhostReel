// Internal. Thin RAII wrapper around one libvpx VP9 decoder instance.
//
// `native_alpha` clips need TWO of these: one for the colour stream and one for the alpha
// stream (the alpha stream is a second VP9 stream; its Y plane is the alpha, full range 0-255).
#pragma once

#include <cstddef>
#include <cstdint>

#include <vpx/vpx_decoder.h>

namespace ac {

struct Vp9Peek {
  bool valid = false;     // the packet header could be parsed
  bool keyframe = false;
  int width = 0;          // only meaningful for keyframes
  int height = 0;
};

// Reads the VP9 frame header only; cheap, does not decode. Never throws.
Vp9Peek PeekVp9(const uint8_t* data, size_t size);

class Vp9Decoder {
 public:
  Vp9Decoder();  // throws ac::Failure(AC_ERR_DECODE) when libvpx cannot be initialised
  ~Vp9Decoder();
  Vp9Decoder(const Vp9Decoder&) = delete;
  Vp9Decoder& operator=(const Vp9Decoder&) = delete;

  // Decodes one packet. Returns the shown frame, valid until the next Decode() or the
  // destructor, or nullptr when the packet produced no shown frame (a hidden frame).
  // Throws ac::Failure(AC_ERR_DECODE) when libvpx rejects the packet, and
  // AC_ERR_UNSUPPORTED for output this core does not handle (not 8-bit 4:2:0, or
  // more than one frame per packet).
  const vpx_image_t* Decode(const uint8_t* data, size_t size);

 private:
  vpx_codec_ctx_t ctx_;
};

}  // namespace ac
