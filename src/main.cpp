#include "core/application.h"
#include "core/log.h"

#include "debugger/debugger.h"

#include <span>

auto main(int argc, char** argv) -> int
{
    const std::span arguments(argv, static_cast<size_t>(argc));

    Hsdbg::ApplicationSpec spec;
    spec.name = "hsdbg";
    spec.width = 1600;
    spec.height = 900;

    Hsdbg::Application app(spec);

    if (arguments.size() > 1)
    {
        if (const auto result = app.debugger().load_target(arguments[1]); !result)
            Hsdbg::Log::error("{}", result.error());
    }

    app.run();

    return 0;
}
