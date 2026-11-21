// [except.spec]: values representable in bool need no narrowing conversion.
constexpr int enabled = 1;
constexpr int disabled = 0;
void on() noexcept(enabled);
void off() noexcept(disabled);
void qualified(bool enabled) noexcept(::enabled);
