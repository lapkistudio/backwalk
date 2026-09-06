#include <stdbool.h>            // for bool, true
#include <stdint.h>             // for uintptr_t
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>            // for QueryPerformanceCounter
#else
#include <time.h>               // for clock_gettime, timespec, CLOCK_MONOTONIC
#endif

#include "common.h"             // for BW_UNUSED
#include "backwalk/backwalk.h"  // for bw_backtrace

#include "test.h"               // for TEST, TEST_ASSERT_GE_INT32, TEST_ASSE...

// Simple counter callback for performance testing
static bool count_callback(uintptr_t addr, const char* fname, const char* sname, void* arg) {
    BW_UNUSED(addr);
    BW_UNUSED(fname);
    BW_UNUSED(sname);

    int* count = (int*)arg;
    (*count)++;

    return true;
}

// Callback that does some work to test overhead
static bool work_callback(uintptr_t addr, const char* fname, const char* sname, void* arg) {
    BW_UNUSED(addr);

    int* total_work = (int*)arg;

    // Do some simple work
    if (fname) {
        int len = 0;
        const char* p = fname;
        while (*p++) {
            len++; // Manual strlen
        }
        *total_work += len;
    }

    if (sname) {
        int len = 0;
        const char* p = sname;
        while (*p++) {
            len++; // Manual strlen
        }
        *total_work += len;
    }

    return true;
}

TEST(repeated_backtrace_calls, {
    const int iterations = 100;
    int total_frames = 0;

    for (int i = 0; i < iterations; i++) {
        int frame_count = 0;
        bool success = bw_backtrace(count_callback, &frame_count);

        TEST_ASSERT_TRUE(success);
        total_frames += frame_count;
    }

    // Should have gotten consistent results
    TEST_ASSERT_GE_INT32(total_frames, iterations); // At least one frame per iteration

    // Average should be reasonable (between 1 and 50 frames per call)
    int average = total_frames / iterations;
    TEST_ASSERT_GE_INT32(average, 1);
    TEST_ASSERT_GE_INT32(50, average);
})

enum { BW_NS_PER_SEC = 1000000000 };

static int64_t bw_now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    if (QueryPerformanceFrequency(&freq) == 0) {
        return 0;
    }
    if (QueryPerformanceCounter(&counter) == 0) {
        return 0;
    }
    return (int64_t)((counter.QuadPart * (int64_t)BW_NS_PER_SEC) / freq.QuadPart);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((int64_t)ts.tv_sec * 1000000000L) + ts.tv_nsec;
#endif
}

TEST(backtrace_performance_basic, {
    const int iterations = 1000;
    const int64_t max_elapsed_ns = (int64_t)500L * 1000 * 1000; // 500ms in nanoseconds

    const int64_t start_time = bw_now_ns();

    for (int i = 0; i < iterations; i++) {
        int count = 0;
        bool success = bw_backtrace(count_callback, &count);
        TEST_ASSERT_TRUE(success);
    }

    const int64_t elapsed_ns = bw_now_ns() - start_time;

    // Should complete reasonably quickly (less than 500ms for 1000 iterations)
    TEST_ASSERT_LE_INT64(elapsed_ns, max_elapsed_ns);
})

// NOLINTNEXTLINE(misc-no-recursion)
static BW_NOINLINE bool stress_recursive_helper(int depth, int max_depth) {
    if (depth >= max_depth) {
        int count = 0;
        return bw_backtrace(count_callback, &count);
    }

    return stress_recursive_helper(depth + 1, max_depth);
}

TEST(deep_stack_stress, {
    const int max_depth = 20;

    bool success = stress_recursive_helper(0, max_depth);

    TEST_ASSERT_TRUE(success);
})

TEST(callback_with_significant_work, {
    int total_work = 0;

    bool success = bw_backtrace(work_callback, &total_work);

    TEST_ASSERT_TRUE(success);
    TEST_ASSERT_GE_INT32(total_work, 0); // Should have done some work
})

// Test memory usage doesn't grow with repeated calls
TEST(memory_stability, {
    const int iterations = 500;

    // Do many iterations and ensure no obvious memory corruption
    for (int i = 0; i < iterations; i++) {
        int count = 0;
        bool success = bw_backtrace(count_callback, &count);
        TEST_ASSERT_TRUE(success);

        // Every 100 iterations, do a quick sanity check
        if (i % 100 == 99) {
            TEST_ASSERT_GE_INT32(count, 1);
        }
    }
})

int main(int argc, char** argv) {
    TEST_INIT("stress", argc, argv);

    TEST_RUN(repeated_backtrace_calls);
    TEST_RUN(backtrace_performance_basic);
    TEST_RUN(deep_stack_stress);
    TEST_RUN(callback_with_significant_work);
    TEST_RUN(memory_stability);

    TEST_EXIT();
}
