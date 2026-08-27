#include "core/application.h"

auto main(int argc, char** argv) -> int
{
    (void)argc;
    (void)argv;

    Hsdbg::ApplicationSpec spec;
    spec.name = "hsdbg";
    spec.width = 1600;
    spec.height = 900;

    Hsdbg::Application app(spec);
    app.run();

    return 0;
}
