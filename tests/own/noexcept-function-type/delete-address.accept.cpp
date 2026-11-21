using Delete = void(void*) noexcept;
constexpr Delete* chosen = &::operator delete;
constexpr void (*relaxed)(void*) = &::operator delete;
static_assert(chosen == relaxed);
