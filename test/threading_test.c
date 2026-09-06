#include <stdbool.h>            // for bool, false, true
#include <stdint.h>             // for uintptr_t
#include <stdlib.h>             // for malloc, free
#include <string.h>             // for memset
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>            // for pthread_join, pthread_create, pthread_t
#include <unistd.h>             // for usleep
#endif

#include "common.h"             // for BW_UNUSED
#include "backwalk/backwalk.h"  // for bw_backtrace, bw_backtrace_cb

#include "test.h"               // for TEST_ERROR_NONZERO, TEST, TEST_ASSERT...

#ifdef _WIN32
typedef HANDLE bw_thread_t;
typedef DWORD bw_tid_t;

typedef struct {
    void* (*start)(void*);
    void* arg;
} bw_thread_wrap_t;

static DWORD WINAPI bw_thread_trampoline(void* p) {
    bw_thread_wrap_t wrap = *(bw_thread_wrap_t*)p;
    free(p);
    (void)wrap.start(wrap.arg);
    return 0;
}

static int bw_thread_spawn(bw_thread_t* thread, void* (*start)(void*), void* arg) {
    bw_thread_wrap_t* wrap = (bw_thread_wrap_t*)malloc(sizeof(*wrap));
    if (wrap == NULL) {
        return 1;
    }
    wrap->start = start;
    wrap->arg = arg;
    *thread = CreateThread(NULL, 0, bw_thread_trampoline, wrap, 0, NULL);
    if (*thread == NULL) {
        free(wrap);
        return 1;
    }
    return 0;
}

static int bw_thread_join(bw_thread_t thread) {
    DWORD rc = WaitForSingleObject(thread, INFINITE);
    (void)CloseHandle(thread);
    return rc == WAIT_OBJECT_0 ? 0 : 1;
}

static void bw_sleep_ms(DWORD ms) {
    Sleep(ms);
}

static bw_tid_t bw_tid(void) {
    return GetCurrentThreadId();
}

static int bw_atomic_add(int* p, int v) {
    return (int)InterlockedAdd((LONG*)p, (LONG)v);
}
#else
typedef pthread_t bw_thread_t;
typedef pthread_t bw_tid_t;

static int bw_thread_spawn(bw_thread_t* thread, void* (*start)(void*), void* arg) {
    return pthread_create(thread, NULL, start, arg);
}

static int bw_thread_join(bw_thread_t thread) {
    return pthread_join(thread, NULL);
}

static void bw_sleep_ms(unsigned ms) {
    BW_UNUSED(usleep((useconds_t)ms * 1000U));
}

static bw_tid_t bw_tid(void) {
    return pthread_self();
}

static int bw_atomic_add(int* p, int v) {
    return __sync_fetch_and_add(p, v);
}
#endif

enum { MAX_THREADS = 8 };
enum { ITERATIONS_PER_THREAD = 100 };

// Thread-safe counter callback
static bool thread_safe_counter(uintptr_t addr, const char* fname, const char* sname, void* arg) {
    BW_UNUSED(addr);
    BW_UNUSED(fname);
    BW_UNUSED(sname);

    volatile int* count = (volatile int*)arg;
    (void)bw_atomic_add((int*)count, 1);

    return true;
}

// Callback that tracks thread-specific data
static bool track_thread_data(uintptr_t addr, const char* fname, const char* sname, void* arg) {
    BW_UNUSED(addr);

    struct thread_stats {
        int total_calls;
        int valid_fnames;
        int valid_snames;
        bw_tid_t thread_id;
    }* stats = (struct thread_stats*)arg;

    (void)bw_atomic_add(&stats->total_calls, 1);

    if (fname && fname[0] != '\0') {
        (void)bw_atomic_add(&stats->valid_fnames, 1);
    }

    if (sname && sname[0] != '\0') {
        (void)bw_atomic_add(&stats->valid_snames, 1);
    }

    return true;
}

// Thread data structure
typedef struct {
    int thread_id;
    int iterations;
    int frames_found;
    bool success;
    volatile int* shared_counter;
} thread_data_t;

// Basic backtrace thread function
static BW_NOINLINE void* backtrace_thread_func(void* arg) {
    thread_data_t* data = (thread_data_t*)arg;

    for (int i = 0; i < data->iterations; i++) {
        int local_count = 0;
        bool result = bw_backtrace(thread_safe_counter, &local_count);

        if (!result) {
            data->success = false;
            return NULL;
        }

        data->frames_found += local_count;

        bw_sleep_ms(1);
    }

    data->success = true;
    return NULL;
}

// Recursive thread function to create deeper stacks
// NOLINTNEXTLINE(misc-no-recursion)
static BW_NOINLINE void* recursive_thread_helper(void* arg, int depth) {
    if (depth <= 0) {
        return backtrace_thread_func(arg);
    }

    return recursive_thread_helper(arg, depth - 1);
}

static BW_NOINLINE void* recursive_backtrace_thread(void* arg) {
    const int recursion_depth = 5;
    return recursive_thread_helper(arg, recursion_depth); // 5 levels of recursion
}

// Function pointer thread to test indirect calls
static BW_NOINLINE void* function_pointer_thread(void* arg) {
    thread_data_t* data = (thread_data_t*)arg;

    // Use function pointer for backtrace call
    bool (*backtrace_func)(bw_backtrace_cb, void*) = bw_backtrace;

    for (int i = 0; i < data->iterations; i++) {
        int local_count = 0;
        bool result = backtrace_func(thread_safe_counter, &local_count);

        if (!result) {
            data->success = false;
            return NULL;
        }

        data->frames_found += local_count;
    }

    data->success = true;
    return NULL;
}

TEST(basic_multithreaded_backtrace, {
    const int num_threads = 4;
    bw_thread_t threads[MAX_THREADS];
    thread_data_t thread_data[MAX_THREADS];
    volatile int shared_counter = 0;

    // Initialize thread data
    for (int i = 0; i < num_threads; i++) {
        thread_data[i].thread_id = i;
        thread_data[i].iterations = 20;
        thread_data[i].frames_found = 0;
        thread_data[i].success = false;
        thread_data[i].shared_counter = &shared_counter;
    }

    // Create threads
    for (int i = 0; i < num_threads; i++) {
        int retval = bw_thread_spawn(&threads[i], backtrace_thread_func, &thread_data[i]);
        TEST_ERROR_NONZERO(retval);
    }

    // Join threads
    for (int i = 0; i < num_threads; i++) {
        TEST_ERROR_NONZERO(bw_thread_join(threads[i]));

        TEST_ASSERT_TRUE(thread_data[i].success);
        TEST_ASSERT_GE_INT32(thread_data[i].frames_found, thread_data[i].iterations);
    }
})

TEST(recursive_multithreaded_backtrace, {
    const int num_threads = 3;
    bw_thread_t threads[MAX_THREADS];
    thread_data_t thread_data[MAX_THREADS];

    // Initialize thread data
    for (int i = 0; i < num_threads; i++) {
        thread_data[i].thread_id = i;
        thread_data[i].iterations = 10;
        thread_data[i].frames_found = 0;
        thread_data[i].success = false;
        thread_data[i].shared_counter = NULL;
    }

    // Create threads with recursive calls
    for (int i = 0; i < num_threads; i++) {
        int retval = bw_thread_spawn(&threads[i], recursive_backtrace_thread, &thread_data[i]);
        TEST_ERROR_NONZERO(retval);
    }

    // Join threads
    for (int i = 0; i < num_threads; i++) {
        TEST_ERROR_NONZERO(bw_thread_join(threads[i]));
        TEST_ASSERT_TRUE(thread_data[i].success);
    }
})

TEST(function_pointer_multithreaded, {
    const int num_threads = 2;
    bw_thread_t threads[MAX_THREADS];
    thread_data_t thread_data[MAX_THREADS];

    // Initialize thread data
    for (int i = 0; i < num_threads; i++) {
        thread_data[i].thread_id = i;
        thread_data[i].iterations = 15;
        thread_data[i].frames_found = 0;
        thread_data[i].success = false;
        thread_data[i].shared_counter = NULL;
    }

    // Create threads using function pointers
    for (int i = 0; i < num_threads; i++) {
        int retval = bw_thread_spawn(&threads[i], function_pointer_thread, &thread_data[i]);
        TEST_ERROR_NONZERO(retval);
    }

    // Join threads
    for (int i = 0; i < num_threads; i++) {
        TEST_ERROR_NONZERO(bw_thread_join(threads[i]));
        TEST_ASSERT_TRUE(thread_data[i].success);
    }
})

// High contention test with many threads
TEST(high_contention_backtrace, {
    const int num_threads = 8;
    bw_thread_t threads[MAX_THREADS];
    thread_data_t thread_data[MAX_THREADS];

    // Initialize thread data for high contention
    for (int i = 0; i < num_threads; i++) {
        thread_data[i].thread_id = i;
        thread_data[i].iterations = 50;
        thread_data[i].frames_found = 0;
        thread_data[i].success = false;
        thread_data[i].shared_counter = NULL;
    }

    // Create many threads simultaneously
    for (int i = 0; i < num_threads; i++) {
        int retval = bw_thread_spawn(&threads[i], backtrace_thread_func, &thread_data[i]);
        TEST_ERROR_NONZERO(retval);
    }

    // Join all threads
    int successful_threads = 0;
    for (int i = 0; i < num_threads; i++) {
        TEST_ERROR_NONZERO(bw_thread_join(threads[i]));

        if (thread_data[i].success) {
            successful_threads++;
        }
    }

    // At least most threads should succeed
    TEST_ASSERT_GE_INT32(successful_threads, num_threads - 1);
})

// Test thread-local data collection
static void* data_collection_thread(void* arg) {
    struct thread_stats {
        int total_calls;
        int valid_fnames;
        int valid_snames;
        bw_tid_t thread_id;
        bool success;
    }* stats = (struct thread_stats*)arg;

    stats->thread_id = bw_tid();
    stats->success = true;

    // Do several backtraces in this thread
    const int num_backtraces = 5;
    for (int i = 0; i < num_backtraces; i++) {
        bool result = bw_backtrace(track_thread_data, stats);
        if (!result) {
            stats->success = false;
            break;
        }
    }

    return NULL;
}

TEST(thread_local_data_collection, {
    const int num_threads = 4;
    bw_thread_t threads[MAX_THREADS];
    struct thread_stats {
        int total_calls;
        int valid_fnames;
        int valid_snames;
        bw_tid_t thread_id;
        bool success;
    } stats[MAX_THREADS];

    // Initialize stats
    for (int i = 0; i < num_threads; i++) {
        BW_UNUSED(memset(&stats[i], 0, sizeof(stats[i])));
    }

    // Create data collection threads
    for (int i = 0; i < num_threads; i++) {
        int retval = bw_thread_spawn(&threads[i], data_collection_thread, &stats[i]);
        TEST_ERROR_NONZERO(retval);
    }

    // Join threads and verify data
    for (int i = 0; i < num_threads; i++) {
        TEST_ERROR_NONZERO(bw_thread_join(threads[i]));

        TEST_ASSERT_TRUE(stats[i].success);

        // Verify thread collected some data
        TEST_ASSERT_GE_INT32(stats[i].total_calls, 5); // At least 5 calls * frames per call
        TEST_ASSERT_TRUE(stats[i].thread_id != 0); // Thread ID should be set
    }
})

int main(int argc, char** argv) {
    TEST_INIT("threading", argc, argv);

    TEST_RUN(basic_multithreaded_backtrace);
    TEST_RUN(recursive_multithreaded_backtrace);
    TEST_RUN(function_pointer_multithreaded);
    TEST_RUN(high_contention_backtrace);
    TEST_RUN(thread_local_data_collection);

    TEST_EXIT();
}
