// a demo target for the flame-chart timeline: a nest of functions that call one
// another so the timeline has real depth. main is the outermost (bottom row),
// each callee stacks a row higher
#include <chrono>
#include <cstdio>
#include <thread>

// innermost: a chunk of busywork so the leaf calls have visible width
auto leaf(int n) -> long
{
    long total = 0;
    for (int i = 0; i < n * 60000; ++i)
        total += i % 7;

    return total;
}

// calls leaf a few times
auto inner(int round) -> long
{
    long total = 0;
    for (int i = 1; i <= 3; ++i)
        total += leaf(round % 4 + i);

    return total;
}

// calls inner, one level below main
auto outer(int round) -> long
{
    const long a = inner(round);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const long b = inner(round + 1);
    return a + b;
}

auto main() -> int
{
    for (int round = 0; round < 20; ++round)
    {
        const long result = outer(round);
        std::printf("round %d -> %ld\n", round, result);
        std::fflush(stdout);

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    std::puts("done");
}
