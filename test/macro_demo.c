// a playground for the macro unwrapper. open this file in hsdbg's source view,
// then hover any highlighted macro for a preview and click it to step through
// the expansion one layer at a time in the macros panel.

#include <stdio.h>

// object-like macros that lean on each other, so unrolling takes real layers:
// KIB -> (1 << 10), MIB -> (KIB * KIB) -> ((1 << 10) * (1 << 10)), and so on
#define KIB (1 << 10)
#define MIB (KIB * KIB)
#define GIB (MIB * KIB)

// function-like macros, including one built out of another
#define SQUARE(x) ((x) * (x))
#define CUBE(x)   (SQUARE(x) * (x))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) MAX(lo, MIN(v, hi))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

// the classic stringize-through-indirection: STR sees its argument raw, XSTR
// expands it first. hover both to see "VERSION" vs "3"
#define VERSION 3
#define STR(x)  #x
#define XSTR(x) STR(x)

// token pasting, direct and through a level of indirection
#define CAT(a, b)  a ## b
#define XCAT(a, b) CAT(a, b)

// a variadic logging macro
#define LOG(level, fmt, ...) fprintf(stderr, "[" level "] " fmt "\n", __VA_ARGS__)

int main(void)
{
    int width  = 5;
    int height = 9;

    printf("a mebibyte is %d bytes\n", MIB);
    printf("square %d, cube %d\n", SQUARE(width), CUBE(width));
    printf("clamped %d\n", CLAMP(height, 0, 8));
    printf("building version %s\n", XSTR(VERSION));

    int XCAT(sum_, 1) = width + height;
    LOG("info", "sum is %d", XCAT(sum_, 1));

    return 0;
}
