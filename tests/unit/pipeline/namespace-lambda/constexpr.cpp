constexpr auto increment = [](int x) { return x + 1; };

int main() { return (*&increment)(41) - 42; }
