// a small demo target for the profiler: allocates and frees memory in slow
// waves so the resident-memory graph has something to show
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

auto main() -> int
{
    std::vector<std::vector<char>> blocks;

    for (int cycle = 0; cycle < 6; ++cycle)
    {
        // ramp up: grab ~40 MB over two seconds
        for (int i = 0; i < 40; ++i)
        {
            blocks.emplace_back(1024 * 1024, static_cast<char>(i));
            std::printf("cycle %d: allocated %zu MB\n", cycle, blocks.size());
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // hold for a moment, then release everything and start over
        std::this_thread::sleep_for(std::chrono::seconds(1));
        blocks.clear();
        blocks.shrink_to_fit();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::puts("done");
}
