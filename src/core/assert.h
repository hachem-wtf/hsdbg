#pragma once

#include "core/log.h"

#include <exception>

#ifdef HSDBG_DIST
    #define HSDBG_ASSERT(condition, message) ((void)0)
#else
    #define HSDBG_ASSERT(condition, message)                                             \
        do                                                                               \
        {                                                                                \
            if (!(condition))                                                            \
            {                                                                            \
                ::Hsdbg::Log::error("assertion failed: {}", #condition);                 \
                ::Hsdbg::Log::error("  {}:{}: {}", __FILE__, __LINE__, message);         \
                std::terminate();                                                        \
            }                                                                            \
        } while (false)
#endif
