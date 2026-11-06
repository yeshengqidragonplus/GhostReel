// Internal. The sidecar metadata (clip.acmeta.json, schema 1): parsing, validation, and the layout it implies for a
// given video. The field list and the rules are documented in docs/m4-notes.md.
#pragma once

#include <cstdint>

#include "alphaclip.h"
#include "common.h"

namespace ac {

struct Meta {
  bool present = false;  // false: no sidecar was given ("auto" mode)
  int schema = 0;        // the sidecar's "schema" (always 1 for a Meta this core accepts); 0 without a sidecar
  int alpha_mode = AC_ALPHA_OPAQUE;
  bool premultiplied = false;
  int color_range = AC_RANGE_UNSPECIFIED;
  int matrix = AC_MATRIX_UNSPECIFIED;
  int content_w = 0;  // 0 = "content_size" was not given
  int content_h = 0;
  bool loop_hint = false;
};

// What the metadata means for a particular file.
struct Layout {
  int alpha_mode = AC_ALPHA_OPAQUE;
  int content_w = 0;
  int content_h = 0;
  int alpha_x = 0;
  int alpha_y = 0;
};

// `json` NULL or "" -> Meta with present == false. Throws ac::Failure (AC_ERR_FORMAT / AC_ERR_UNSUPPORTED / AC_ERR_OOM).
Meta ParseMeta(const char* json);

// True when the alpha stream of the file must be ignored (opaque, packed_*): decided before the file is parsed.
bool MetaIgnoresAlphaStream(const Meta& meta);

// Checks the metadata against the file and works out the picture and alpha rectangles. Throws AC_ERR_FORMAT when
// they contradict each other. `stream_has_alpha` = the file carries an alpha stream (whether or not it is decoded).
Layout ResolveLayout(const Meta& meta, int video_w, int video_h, bool stream_has_alpha);

}  // namespace ac
