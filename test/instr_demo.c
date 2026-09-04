// a fully instrumented demo: built with -finstrument-functions so EVERY function
// here is traced automatically, no hand-picking. kept in plain C with only
// printf and nanosleep so the trace is exactly these functions
#include <stdio.h>
#include <time.h>

static void nap(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static long leaf(int n)
{
    long total = 0;
    for (int i = 0; i < n * 40000; ++i)
        total += i % 7;

    return total;
}

static long inner(int round)
{
    long total = 0;
    for (int i = 1; i <= 3; ++i)
        total += leaf(round % 4 + i);

    return total;
}

static long outer(int round)
{
    const long a = inner(round);
    nap(20);
    const long b = inner(round + 1);
    return a + b;
}

int main(void)
{
    for (int round = 0; round < 15; ++round)
    {
        const long result = outer(round);
        printf("round %d -> %ld\n", round, result);
        fflush(stdout);
        nap(120);
    }

    puts("done");
    return 0;
}
