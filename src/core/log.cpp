#include "core/log.h"

// there is no standard way to ask whether a stream is a terminal
#ifdef HSDBG_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>

    #include <io.h>
#else
    #include <unistd.h>
#endif

namespace Hsdbg::Log::Detail
{
    namespace
    {
        constexpr int STDOUT_DESCRIPTOR = 1;
        constexpr int STDERR_DESCRIPTOR = 2;

#ifdef HSDBG_WINDOWS
        // a windows console prints escapes literally until it is asked to
        // interpret them, so a console that refuses gets the plain format
        auto probe(bool to_stderr) -> bool
        {
            if (_isatty(to_stderr ? STDERR_DESCRIPTOR : STDOUT_DESCRIPTOR) == 0)
                return false;

            const HANDLE console = GetStdHandle(to_stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);

            if (console == nullptr || console == INVALID_HANDLE_VALUE)
                return false;

            DWORD mode = 0;

            if (GetConsoleMode(console, &mode) == 0)
                return false;

            if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0)
                return true;

            return SetConsoleMode(console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
        }
#else
        auto probe(bool to_stderr) -> bool
        {
            return isatty(to_stderr ? STDERR_DESCRIPTOR : STDOUT_DESCRIPTOR) != 0;
        }
#endif
    }

    auto is_terminal(bool to_stderr) -> bool
    {
        static const bool out = probe(false);
        static const bool err = probe(true);

        return to_stderr ? err : out;
    }
}
