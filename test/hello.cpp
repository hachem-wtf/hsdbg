#include <print>

auto add(int a, int b) -> int
{
    int result = a+b;
    return result;
}

auto main() -> int
{
    int a = 50;
    int b = 67;
    int c = add(a, b);

    std::println("{} + {} = {}", a, b, c);
}
