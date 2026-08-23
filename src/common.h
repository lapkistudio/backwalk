#ifndef BW_COMMON_H
#define BW_COMMON_H

#ifndef _WIN32
#include <pthread.h>  // for pthread_cond_t, pthread_mutex_t
#endif
#ifndef __cplusplus
#include <stdbool.h>  // for bool
#endif

#define BW_UNUSED(expr) (void)(expr)

#define BW_ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

#if defined(_MSC_VER)
#define BW_NOINLINE __declspec(noinline)
#else
#define BW_NOINLINE __attribute__((noinline))
#endif

#if defined(__clang__)
#define BW_NO_SANITIZE_ADDRESS __attribute__((no_sanitize("address")))
#elif defined(_MSC_VER)
#define BW_NO_SANITIZE_ADDRESS __declspec(no_sanitize_address)
#else
#define BW_NO_SANITIZE_ADDRESS
#endif

#ifndef _WIN32
typedef struct {
    pthread_cond_t cv;
    pthread_mutex_t mtx;
    bool state;
} ticket_t;

void ticket_init(ticket_t* ticket);

void ticket_signal(ticket_t* ticket);

void ticket_wait(ticket_t* ticket);
#endif

#endif // BW_COMMON_H
