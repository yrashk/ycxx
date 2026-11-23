constexpr unsigned leaf(unsigned line = __builtin_LINE()) noexcept { return line; }
constexpr unsigned outer(unsigned line = leaf()) noexcept { return line; }

#line 80
constexpr unsigned wrapper() noexcept { return leaf(); }

unsigned reported(unsigned line = __builtin_LINE()) noexcept { return line; }
