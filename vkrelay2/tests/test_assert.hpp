// Tiny dependency-free assertion helpers. Each test is its own executable with
// an int main(); CTest treats a non-zero return as failure.
#ifndef VKRELAY2_TESTS_TEST_ASSERT_HPP
#define VKRELAY2_TESTS_TEST_ASSERT_HPP

#include <cstdio>
#include <sstream>
#include <string>

namespace vkr::test {

struct Counters {
    int checks = 0;
    int failures = 0;
};

inline Counters& counters() {
    static Counters c;
    return c;
}

inline void report_fail(const char* file, int line, const std::string& msg) {
    ++counters().failures;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, msg.c_str());
}

template <typename A, typename B>
void check_eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    ++counters().checks;
    if (!(a == b)) {
        std::ostringstream os;
        os << "CHECK_EQ failed: " << ea << " == " << eb << "  (" << a << " vs " << b << ")";
        report_fail(file, line, os.str());
    }
}

inline int finish(const char* name) {
    std::fprintf(stderr, "%s: %d checks, %d failures\n", name, counters().checks,
                 counters().failures);
    return counters().failures == 0 ? 0 : 1;
}

} // namespace vkr::test

#define VKR_CHECK(cond)                                                                            \
    do {                                                                                           \
        ++::vkr::test::counters().checks;                                                          \
        if (!(cond)) {                                                                             \
            ::vkr::test::report_fail(__FILE__, __LINE__, "CHECK failed: " #cond);                  \
        }                                                                                          \
    } while (0)

#define VKR_CHECK_EQ(a, b) ::vkr::test::check_eq((a), (b), #a, #b, __FILE__, __LINE__)

#endif // VKRELAY2_TESTS_TEST_ASSERT_HPP
