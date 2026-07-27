// House test harness: CHECK macro + failure count as exit code. Test exes
// print "all <Suite> tests passed" and return 0, or list FAIL lines.
#pragma once
#include <cstdio>

inline int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                             \
    do {                                                                  \
        const double _va = (a), _vb = (b);                                \
        if (!(_va >= _vb - (eps) && _va <= _vb + (eps))) {                \
            std::printf("FAIL %s:%d: %s=%.9f vs %s=%.9f\n", __FILE__,     \
                        __LINE__, #a, _va, #b, _vb);                      \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

#define TEST_MAIN(name)                                 \
    int main() {                                        \
        RunTests();                                     \
        if (g_failures == 0)                            \
            std::printf("all " name " tests passed\n"); \
        return g_failures;                              \
    }
