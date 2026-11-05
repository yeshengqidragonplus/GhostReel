// M0 spike: the smallest possible "core" shared library.
//
// It exists only to prove that each target toolchain can turn C++17 code that uses the standard
// library into a shared library with a C ABI and an exported symbol. It contains no product logic
// and will be deleted once the real core/ library exists.
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define AC_SPIKE_EXPORT extern "C" __declspec(dllexport)
#else
#define AC_SPIKE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

struct Pair {
    int32_t a;
    int32_t b;
};

Pair make_pair_values(int32_t seed) { return {seed, seed * 2}; }

}  // namespace

// Returns 0 when the C++17 features used below behave as expected, non-zero otherwise.
AC_SPIKE_EXPORT int32_t ac_spike_selftest(void) {
    auto values = std::make_unique<std::vector<int32_t>>();
    for (int32_t i = 1; i <= 4; ++i) values->push_back(i);

    int32_t sum = 0;
    for (int32_t v : *values) sum += v;  // 10

    const auto [a, b] = make_pair_values(3);  // structured bindings (C++17)

    std::string label = "ac";
    label += "_spike";

    int32_t extra = 0;
    if constexpr (sizeof(void*) >= 4) extra = 1;  // if constexpr (C++17)

    const bool ok = sum == 10 && a == 3 && b == 6 && label.size() == 8 && extra == 1;
    return ok ? 0 : 1;
}

// Pointer width in bits, so a caller can tell which ABI it actually loaded.
AC_SPIKE_EXPORT int32_t ac_spike_pointer_bits(void) { return static_cast<int32_t>(sizeof(void*) * 8); }
