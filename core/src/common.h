// Internal. Shared limits and the error type used inside the core.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "alphaclip.h"

namespace ac {

// Hard limits: anything beyond them is rejected with an error instead of being allocated.
constexpr int kMaxDimension = 8192;                  // per side
constexpr int64_t kMaxPixels = 7680LL * 4320;        // 8K UHD
constexpr uint64_t kMaxFileBytes = 1ull << 30;       // ac_open_file reads the whole file
constexpr size_t kMaxPackets = 1u << 20;             // bounds the packet index

// Used inside the core instead of status plumbing. It is caught at the C ABI boundary
// (src/api.cpp) and turned into an ac_status; it never leaves the library.
class Failure : public std::runtime_error {
 public:
  Failure(ac_status status, const std::string& message)
      : std::runtime_error(message), status_(status) {}
  ac_status status() const { return status_; }

 private:
  ac_status status_;
};

}  // namespace ac
