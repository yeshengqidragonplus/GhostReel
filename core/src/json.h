// Internal. A small strict JSON reader (RFC 8259) for the sidecar metadata. No dependency; the input size,
// nesting depth, string length and member count are bounded, so hostile input cannot make it allocate much
// or overflow the stack.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ac {

constexpr size_t kMaxJsonBytes = 64 * 1024;
constexpr int kMaxJsonDepth = 8;

struct JsonValue {
  enum class Type { Null, Bool, Number, String, Array, Object };

  Type type = Type::Null;
  bool boolean = false;
  bool is_integer = false;  // a Number written without fraction/exponent that fits in int64
  int64_t integer = 0;      // valid when is_integer
  double number = 0;        // every Number (the integer value as a double when is_integer)
  std::string string;
  std::vector<JsonValue> array;
  std::vector<std::pair<std::string, JsonValue>> object;  // file order; duplicate keys are rejected

  // Objects only: the member with this key, or nullptr.
  const JsonValue* Find(const char* key) const;
};

// Parses a complete JSON text (a leading UTF-8 byte-order mark is skipped, because Windows tools write one).
// Throws ac::Failure: AC_ERR_OOM when `len` exceeds kMaxJsonBytes, AC_ERR_FORMAT for anything malformed.
JsonValue ParseJson(const char* text, size_t len);

}  // namespace ac
