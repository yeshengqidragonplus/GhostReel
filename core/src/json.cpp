#include "json.h"

#include <cmath>
#include <cstring>
#include <limits>

#include "common.h"

namespace ac {
namespace {

constexpr size_t kMaxStringBytes = 4096;
constexpr size_t kMaxMembers = 64;  // per object / array

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

void AppendUtf8(std::string* out, uint32_t cp) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

class Parser {
 public:
  Parser(const char* s, size_t n) : begin_(s), p_(s), end_(s + n) {}

  JsonValue Document() {
    if (end_ - p_ >= 3 && static_cast<uint8_t>(p_[0]) == 0xEF && static_cast<uint8_t>(p_[1]) == 0xBB &&
        static_cast<uint8_t>(p_[2]) == 0xBF) {
      p_ += 3;
    }
    SkipWs();
    JsonValue v = Value(0);
    SkipWs();
    if (p_ != end_) Fail("unexpected content after the JSON value");
    return v;
  }

 private:
  [[noreturn]] void Fail(const std::string& what) const {
    throw Failure(AC_ERR_FORMAT, "sidecar metadata is not valid JSON: " + what + " (at byte " +
                                     std::to_string(p_ - begin_) + ")");
  }

  bool More() const { return p_ < end_; }

  void SkipWs() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
  }

  void Literal(const char* word) {
    const size_t n = std::strlen(word);
    if (static_cast<size_t>(end_ - p_) < n || std::memcmp(p_, word, n) != 0) Fail("invalid literal");
    p_ += n;
  }

  JsonValue Value(int depth) {
    if (depth > kMaxJsonDepth) Fail("nested too deeply");
    if (!More()) Fail("unexpected end of the text");
    JsonValue v;
    const char c = *p_;
    if (c == '{') {
      ParseObject(&v, depth);
    } else if (c == '[') {
      ParseArray(&v, depth);
    } else if (c == '"') {
      v.type = JsonValue::Type::String;
      ParseString(&v.string);
    } else if (c == 't') {
      Literal("true");
      v.type = JsonValue::Type::Bool;
      v.boolean = true;
    } else if (c == 'f') {
      Literal("false");
      v.type = JsonValue::Type::Bool;
    } else if (c == 'n') {
      Literal("null");
    } else if (c == '-' || IsDigit(c)) {
      ParseNumber(&v);
    } else {
      Fail("unexpected character");
    }
    return v;
  }

  void ParseObject(JsonValue* v, int depth) {
    v->type = JsonValue::Type::Object;
    ++p_;  // '{'
    SkipWs();
    if (More() && *p_ == '}') {
      ++p_;
      return;
    }
    for (;;) {
      SkipWs();
      if (!More() || *p_ != '"') Fail("expected a string key");
      std::string key;
      ParseString(&key);
      for (const auto& m : v->object) {
        if (m.first == key) Fail("duplicate key '" + key + "'");
      }
      SkipWs();
      if (!More() || *p_ != ':') Fail("expected ':'");
      ++p_;
      SkipWs();
      if (v->object.size() >= kMaxMembers) Fail("too many members");
      JsonValue member = Value(depth + 1);
      v->object.emplace_back(std::move(key), std::move(member));
      SkipWs();
      if (!More()) Fail("unterminated object");
      if (*p_ == ',') {
        ++p_;
        continue;
      }
      if (*p_ == '}') {
        ++p_;
        return;
      }
      Fail("expected ',' or '}'");
    }
  }

  void ParseArray(JsonValue* v, int depth) {
    v->type = JsonValue::Type::Array;
    ++p_;  // '['
    SkipWs();
    if (More() && *p_ == ']') {
      ++p_;
      return;
    }
    for (;;) {
      SkipWs();
      if (v->array.size() >= kMaxMembers) Fail("too many elements");
      v->array.push_back(Value(depth + 1));
      SkipWs();
      if (!More()) Fail("unterminated array");
      if (*p_ == ',') {
        ++p_;
        continue;
      }
      if (*p_ == ']') {
        ++p_;
        return;
      }
      Fail("expected ',' or ']'");
    }
  }

  uint32_t Hex4() {
    if (end_ - p_ < 4) Fail("incomplete \\u escape");
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = *p_++;
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
      else Fail("invalid \\u escape");
    }
    return v;
  }

  void ParseEscape(std::string* out) {
    if (!More()) Fail("unterminated escape");
    const char e = *p_++;
    switch (e) {
      case '"': out->push_back('"'); break;
      case '\\': out->push_back('\\'); break;
      case '/': out->push_back('/'); break;
      case 'b': out->push_back('\b'); break;
      case 'f': out->push_back('\f'); break;
      case 'n': out->push_back('\n'); break;
      case 'r': out->push_back('\r'); break;
      case 't': out->push_back('\t'); break;
      case 'u': {
        uint32_t cp = Hex4();
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          if (end_ - p_ < 6 || p_[0] != '\\' || p_[1] != 'u') Fail("unpaired surrogate escape");
          p_ += 2;
          const uint32_t lo = Hex4();
          if (lo < 0xDC00 || lo > 0xDFFF) Fail("unpaired surrogate escape");
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          Fail("unpaired surrogate escape");
        }
        if (cp == 0) Fail("NUL character in a string");
        AppendUtf8(out, cp);
        break;
      }
      default: Fail("invalid escape");
    }
  }

  // Copies one multi-byte UTF-8 sequence, rejecting overlong forms, surrogates and values above U+10FFFF.
  void CopyUtf8(std::string* out) {
    const uint8_t b0 = static_cast<uint8_t>(*p_);
    int n = 0;
    uint8_t lo = 0x80, hi = 0xBF;
    if (b0 >= 0xC2 && b0 <= 0xDF) n = 2;
    else if (b0 == 0xE0) { n = 3; lo = 0xA0; }
    else if (b0 >= 0xE1 && b0 <= 0xEC) n = 3;
    else if (b0 == 0xED) { n = 3; hi = 0x9F; }
    else if (b0 >= 0xEE && b0 <= 0xEF) n = 3;
    else if (b0 == 0xF0) { n = 4; lo = 0x90; }
    else if (b0 >= 0xF1 && b0 <= 0xF3) n = 4;
    else if (b0 == 0xF4) { n = 4; hi = 0x8F; }
    else Fail("invalid UTF-8");
    if (end_ - p_ < n) Fail("invalid UTF-8");
    const uint8_t b1 = static_cast<uint8_t>(p_[1]);
    if (b1 < lo || b1 > hi) Fail("invalid UTF-8");
    for (int i = 2; i < n; ++i) {
      if ((static_cast<uint8_t>(p_[i]) & 0xC0) != 0x80) Fail("invalid UTF-8");
    }
    out->append(p_, static_cast<size_t>(n));
    p_ += n;
  }

  void ParseString(std::string* out) {
    ++p_;  // opening quote
    for (;;) {
      if (!More()) Fail("unterminated string");
      const uint8_t c = static_cast<uint8_t>(*p_);
      if (c == '"') {
        ++p_;
        return;
      }
      if (c < 0x20) Fail("control character inside a string");
      if (out->size() >= kMaxStringBytes) Fail("string too long");
      if (c == '\\') {
        ++p_;
        ParseEscape(out);
      } else if (c < 0x80) {
        out->push_back(static_cast<char>(c));
        ++p_;
      } else {
        CopyUtf8(out);
      }
    }
  }

  void ParseNumber(JsonValue* v) {
    v->type = JsonValue::Type::Number;
    const bool neg = *p_ == '-';
    if (neg) ++p_;
    if (!More() || !IsDigit(*p_)) Fail("invalid number");

    // Locale-independent: no strtod. Up to 40 significant digits are kept; the rest only moves the exponent.
    long double mant = 0;
    int sig = 0;
    int exp10 = 0;
    uint64_t ival = 0;
    bool ival_ok = true;
    auto digit = [&](int d, bool fraction) {
      if (ival_ok) {
        if (ival > (std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(d)) / 10) ival_ok = false;
        else ival = ival * 10 + static_cast<uint64_t>(d);
      }
      if (sig < 40) {
        mant = mant * 10 + d;
        ++sig;
        if (fraction) --exp10;
      } else if (!fraction) {
        ++exp10;
      }
    };

    bool integer_syntax = true;
    if (*p_ == '0') {
      digit(0, false);
      ++p_;
      if (More() && IsDigit(*p_)) Fail("leading zero in a number");
    } else {
      while (More() && IsDigit(*p_)) {
        digit(*p_ - '0', false);
        ++p_;
      }
    }
    if (More() && *p_ == '.') {
      integer_syntax = false;
      ++p_;
      if (!More() || !IsDigit(*p_)) Fail("invalid number");
      while (More() && IsDigit(*p_)) {
        digit(*p_ - '0', true);
        ++p_;
      }
    }
    if (More() && (*p_ == 'e' || *p_ == 'E')) {
      integer_syntax = false;
      ++p_;
      bool eneg = false;
      if (More() && (*p_ == '+' || *p_ == '-')) {
        eneg = *p_ == '-';
        ++p_;
      }
      if (!More() || !IsDigit(*p_)) Fail("invalid number");
      int e = 0;
      while (More() && IsDigit(*p_)) {
        if (e < 100000) e = e * 10 + (*p_ - '0');
        ++p_;
      }
      exp10 += eneg ? -e : e;
    }

    if (integer_syntax && ival_ok) {
      constexpr uint64_t kMinMagnitude = 9223372036854775808ull;  // |INT64_MIN|
      if (neg ? ival <= kMinMagnitude : ival <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        v->is_integer = true;
        v->integer = neg ? (ival == kMinMagnitude ? std::numeric_limits<int64_t>::min()
                                                  : -static_cast<int64_t>(ival))
                         : static_cast<int64_t>(ival);
      }
    }
    double value = 0;
    if (mant != 0) {
      if (exp10 > 400) Fail("number out of range");
      if (exp10 >= -800) {
        const long double scaled = mant * std::pow(10.0L, static_cast<long double>(exp10));
        if (!std::isfinite(scaled) || scaled > 1e300L) Fail("number out of range");
        value = static_cast<double>(scaled);
      }
    }
    v->number = neg ? -value : value;
  }

  const char* begin_;
  const char* p_;
  const char* end_;
};

}  // namespace

const JsonValue* JsonValue::Find(const char* key) const {
  if (type != Type::Object) return nullptr;
  for (const auto& m : object) {
    if (m.first == key) return &m.second;
  }
  return nullptr;
}

JsonValue ParseJson(const char* text, size_t len) {
  if (len > kMaxJsonBytes) {
    throw Failure(AC_ERR_OOM, "sidecar metadata is larger than " + std::to_string(kMaxJsonBytes) + " bytes");
  }
  return Parser(text, len).Document();
}

}  // namespace ac
