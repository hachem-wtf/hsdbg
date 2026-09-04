// a plain, un-instrumented target for the sampling profiler: NOTHING added to the
// build, no flags, no linked runtime. functions do enough work (tens of ms) that
// the sampler catches them. compile normally:  clang++ -g sample_demo.cpp -o sample_demo
#include <cstdio>

// a spin of roughly the requested milliseconds of busywork
static volatile long sink = 0;
static void burn(int ms)
{
    // ~2 million iterations per ms is a rough guess; exact timing does not matter
    for (long i = 0; i < ms * 800000L; ++i)
        sink += i % 7;
}

static void deep(int round)
{
    burn(25 + round % 3 * 15);
}

static void phase_b(int round)
{
    burn(20);
    deep(round);
}

static void phase_a(int round)
{
    burn(40);
}

int main()
{
    for (int round = 0; round < 400; ++round)
    {
        phase_a(round);
        phase_b(round);

        if (round % 10 == 0)
        {
            printf("round %d\n", round);
            fflush(stdout);
        }
    }

    puts("done");
    return 0;
}
