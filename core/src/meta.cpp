#include "meta.h"

#include <cmath>
#include <cstring>
#include <string>

#include "json.h"

namespace ac {
namespace {

[[noreturn]] void Bad(const std::string& what) {
  throw Failure(AC_ERR_FORMAT, "sidecar metadata: " + what);
}

[[noreturn]] void Unsupported(const std::string& what) {
  throw Failure(AC_ERR_UNSUPPORTED, "sidecar metadata: " + what);
}

bool OptionalBool(const JsonValue& o, const char* key, bool fallback) {
  const JsonValue* v = o.Find(key);
  if (v == nullptr) return fallback;
  if (v->type != JsonValue::Type::Bool) Bad(std::string("'") + key + "' must be true or false");
  return v->boolean;
}

const std::string* OptionalString(const JsonValue& o, const char* key) {
  const JsonValue* v = o.Find(key);
  if (v == nullptr) return nullptr;
  if (v->type != JsonValue::Type::String) Bad(std::string("'") + key + "' must be a string");
  return &v->string;
}

int ParseAlphaMode(const std::string& s) {
  if (s == "opaque") return AC_ALPHA_OPAQUE;
  if (s == "packed_right") return AC_ALPHA_PACKED_RIGHT;
  if (s == "packed_bottom") return AC_ALPHA_PACKED_BOTTOM;
  if (s == "native_alpha") return AC_ALPHA_NATIVE;
  if (s == "keyed") Unsupported("alpha_mode 'keyed' is not implemented yet");
  Bad("unknown alpha_mode '" + s + "' (expected opaque, packed_right, packed_bottom or native_alpha)");
}

// edge.shrink / feather / denoise are reserved for the edge post-processing pass, which does not exist yet.
// Values that would change the picture are refused rather than silently ignored.
void CheckEdge(const JsonValue& root) {
  const JsonValue* edge = root.Find("edge");
  if (edge == nullptr) return;
  if (edge->type != JsonValue::Type::Object) Bad("'edge' must be an object");
  for (const char* key : {"shrink", "feather", "denoise"}) {
    const JsonValue* v = edge->Find(key);
    if (v == nullptr) continue;
    if (v->type != JsonValue::Type::Number || !std::isfinite(v->number) || v->number < 0) {
      Bad(std::string("'edge.") + key + "' must be a number >= 0");
    }
    if (v->number != 0) Unsupported("edge post-processing is not implemented yet ('edge." + std::string(key) + "' must be 0)");
  }
}

}  // namespace

Meta ParseMeta(const char* json) {
  Meta m;
  if (json == nullptr || json[0] == '\0') return m;
  // The text is NUL-terminated: find its end without reading past the size limit.
  const void* nul = std::memchr(json, '\0', kMaxJsonBytes + 1);
  if (nul == nullptr) {
    throw Failure(AC_ERR_OOM, "sidecar metadata is larger than " + std::to_string(kMaxJsonBytes) + " bytes");
  }
  const size_t len = static_cast<size_t>(static_cast<const char*>(nul) - json);
  const JsonValue root = ParseJson(json, len);
  if (root.type != JsonValue::Type::Object) Bad("the top level must be an object");

  const JsonValue* schema = root.Find("schema");
  if (schema == nullptr) Bad("'schema' is required");
  if (schema->type != JsonValue::Type::Number || !schema->is_integer) Bad("'schema' must be an integer");
  if (schema->integer != 1) {
    Unsupported("schema " + std::to_string(schema->integer) + " is not supported (this core reads schema 1)");
  }

  const std::string* mode = OptionalString(root, "alpha_mode");
  if (mode == nullptr) Bad("'alpha_mode' is required");
  m.present = true;
  m.alpha_mode = ParseAlphaMode(*mode);
  m.premultiplied = OptionalBool(root, "premultiplied", false);
  m.loop_hint = OptionalBool(root, "loop_hint", false);

  if (const std::string* r = OptionalString(root, "color_range")) {
    if (*r == "limited") m.color_range = AC_RANGE_LIMITED;
    else if (*r == "full") m.color_range = AC_RANGE_FULL;
    else Bad("'color_range' must be \"limited\" or \"full\"");
  }
  if (const std::string* x = OptionalString(root, "matrix")) {
    if (*x == "bt601") m.matrix = AC_MATRIX_BT601;
    else if (*x == "bt709") m.matrix = AC_MATRIX_BT709;
    else Bad("'matrix' must be \"bt601\" or \"bt709\"");
  }

  if (const JsonValue* cs = root.Find("content_size")) {
    if (cs->type != JsonValue::Type::Array || cs->array.size() != 2) Bad("'content_size' must be [width, height]");
    for (const JsonValue& e : cs->array) {
      if (e.type != JsonValue::Type::Number || !e.is_integer || e.integer < 1 || e.integer > kMaxDimension) {
        Bad("'content_size' must hold two integers between 1 and " + std::to_string(kMaxDimension));
      }
    }
    m.content_w = static_cast<int>(cs->array[0].integer);
    m.content_h = static_cast<int>(cs->array[1].integer);
  }

  CheckEdge(root);
  return m;
}

bool MetaIgnoresAlphaStream(const Meta& meta) {
  return meta.present && meta.alpha_mode != AC_ALPHA_NATIVE;
}

Layout ResolveLayout(const Meta& meta, int video_w, int video_h, bool stream_has_alpha) {
  Layout l;
  l.content_w = video_w;
  l.content_h = video_h;
  if (!meta.present) {  // auto
    l.alpha_mode = stream_has_alpha ? AC_ALPHA_NATIVE : AC_ALPHA_OPAQUE;
    return l;
  }
  l.alpha_mode = meta.alpha_mode;
  const bool given = meta.content_w > 0;
  switch (meta.alpha_mode) {
    case AC_ALPHA_OPAQUE:
    case AC_ALPHA_NATIVE:
      if (meta.alpha_mode == AC_ALPHA_NATIVE && !stream_has_alpha) {
        Bad("alpha_mode is native_alpha but the file carries no alpha stream");
      }
      if (given && (meta.content_w != video_w || meta.content_h != video_h)) {
        Bad("content_size " + std::to_string(meta.content_w) + "x" + std::to_string(meta.content_h) +
            " differs from the video size " + std::to_string(video_w) + "x" + std::to_string(video_h) +
            " (only the packed modes use a smaller picture)");
      }
      return l;
    default:
      break;
  }

  // packed_right / packed_bottom
  const bool right = meta.alpha_mode == AC_ALPHA_PACKED_RIGHT;
  const char* name = right ? "packed_right" : "packed_bottom";
  if (stream_has_alpha) {
    Bad(std::string("alpha_mode is ") + name + " but the file also carries an alpha stream");
  }
  int cw = meta.content_w;
  int ch = meta.content_h;
  if (!given) {
    cw = right ? video_w / 2 : video_w;
    ch = right ? video_h : video_h / 2;
    if ((right ? video_w % 2 : video_h % 2) != 0) {
      Bad(std::string(name) + ": content_size is missing and the video size cannot be halved");
    }
  }
  // Even sizes: the picture's 4:2:0 chroma samples then never straddle the border with the alpha image.
  if (cw < 2 || ch < 2 || cw % 2 != 0 || ch % 2 != 0) {
    Bad(std::string(name) + ": the picture size " + std::to_string(cw) + "x" + std::to_string(ch) +
        " must be even and at least 2x2");
  }
  const int need_w = right ? 2 * cw : cw;
  const int need_h = right ? ch : 2 * ch;
  if (need_w > video_w || need_h > video_h) {
    Bad(std::string(name) + ": a " + std::to_string(cw) + "x" + std::to_string(ch) +
        " picture plus its alpha image needs " + std::to_string(need_w) + "x" + std::to_string(need_h) +
        " but the video is " + std::to_string(video_w) + "x" + std::to_string(video_h));
  }
  l.content_w = cw;
  l.content_h = ch;
  l.alpha_x = right ? cw : 0;
  l.alpha_y = right ? 0 : ch;
  return l;
}

}  // namespace ac
