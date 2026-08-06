#pragma once

#include <cstdio>
#include <cstdlib>

namespace dhft::detail {

[[noreturn]] inline void check_failed(const char* expr, const char* file, int line,
                                      const char* msg) noexcept {
    std::fprintf(stderr, "DHFT_CHECK failed: %s\n  at %s:%d\n", expr, file, line);
    if (msg != nullptr) {
        std::fprintf(stderr, "  %s\n", msg);
    }
    std::fflush(stderr);
    std::abort();
}

} // namespace dhft::detail

#define DHFT_CHECK(cond)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            ::dhft::detail::check_failed(#cond, __FILE__, __LINE__, nullptr);             \
        }                                                                                \
    } while (false)

#define DHFT_CHECK_MSG(cond, msg)                                                        \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            ::dhft::detail::check_failed(#cond, __FILE__, __LINE__, (msg));               \
        }                                                                                \
    } while (false)
