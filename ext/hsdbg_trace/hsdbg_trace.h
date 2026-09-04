// hsdbg function-trace runtime.
//
// Build your program with -finstrument-functions and link this in; the compiler
// then calls the enter/exit hooks below on every function, and they append a
// timestamped record to a fixed ring buffer. hsdbg reads that buffer straight
// out of the live process and turns it into the flame-chart timeline, so no
// breakpoints and no hand-picking of functions are needed.
//
//   clang -finstrument-functions -g your_prog.c hsdbg_trace.c -o your_prog
//
// The record layout and the three exported symbols are the contract hsdbg reads
// against; keep them in sync with the reader on the debugger side.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    enum
    {
        HSDBG_TRACE_ENTER = 0,
        HSDBG_TRACE_EXIT = 1,

        // 1,048,576 records ~= 32 MB; plenty for a short profiled run
        HSDBG_TRACE_CAPACITY = 1u << 20,
    };

    // 32 bytes, naturally aligned so hsdbg can read the array with one memory
    // fetch and cast without worrying about padding differences
    struct HsdbgTraceRecord
    {
        uint64_t timestamp_ns; // CLOCK_MONOTONIC nanoseconds
        uint64_t function;     // address of the entered/exited function
        uint64_t thread_id;    // pthread_threadid_np of the running thread
        uint32_t kind;         // HSDBG_TRACE_ENTER or HSDBG_TRACE_EXIT
        uint32_t reserved;
    };

    // the ring buffer and how far it has been filled. hsdbg reads hsdbg_trace_head
    // first, then the records below it; head is published only after a record's
    // fields are fully written
    extern struct HsdbgTraceRecord hsdbg_trace_records[HSDBG_TRACE_CAPACITY];
    extern const uint64_t hsdbg_trace_capacity;
    extern uint64_t hsdbg_trace_head; // total records ever written (monotonic)

#ifdef __cplusplus
}
#endif
