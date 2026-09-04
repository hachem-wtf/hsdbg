#include "hsdbg_trace.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// the hooks and their helpers must never instrument themselves, or every record
// would recurse into another record
#define NO_INSTR __attribute__((no_instrument_function))

struct HsdbgTraceRecord hsdbg_trace_records[HSDBG_TRACE_CAPACITY];
const uint64_t hsdbg_trace_capacity = HSDBG_TRACE_CAPACITY;
uint64_t hsdbg_trace_head = 0;

// slots are hedged out with a private atomic so concurrent threads never claim
// the same one; hsdbg_trace_head is the separate, reader-facing high-water mark
static _Atomic uint64_t g_next = 0;

static NO_INSTR uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static NO_INSTR uint64_t current_thread(void)
{
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);
    return id;
}

static NO_INSTR void record(uint32_t kind, void* function)
{
    const uint64_t slot = atomic_fetch_add_explicit(&g_next, 1, memory_order_relaxed);

    struct HsdbgTraceRecord* entry = &hsdbg_trace_records[slot % HSDBG_TRACE_CAPACITY];
    entry->timestamp_ns = now_ns();
    entry->function = (uint64_t)(uintptr_t)function;
    entry->thread_id = current_thread();
    entry->kind = kind;
    entry->reserved = 0;

    // publish only once the record is complete, so a reader that sees the new
    // head always finds fully written fields
    atomic_store_explicit((_Atomic uint64_t*)&hsdbg_trace_head, slot + 1, memory_order_release);
}

NO_INSTR void __cyg_profile_func_enter(void* this_fn, void* call_site)
{
    (void)call_site;
    record(HSDBG_TRACE_ENTER, this_fn);
}

NO_INSTR void __cyg_profile_func_exit(void* this_fn, void* call_site)
{
    (void)call_site;
    record(HSDBG_TRACE_EXIT, this_fn);
}

// opt-in sanity check when running the target on its own, outside hsdbg
__attribute__((destructor)) NO_INSTR static void hsdbg_trace_report(void)
{
    if (getenv("HSDBG_TRACE_DEBUG"))
        fprintf(stderr, "[hsdbg_trace] %llu records written\n", (unsigned long long)hsdbg_trace_head);
}
