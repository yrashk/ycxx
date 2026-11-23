// #975 [basic.life], [expr.const]
// XFAIL: gcc GCC 16 accepts the call after the receiver's lifetime when the body ignores this.
struct Empty { constexpr unsigned value() const { return 7; } };
consteval Empty make() { Empty value{}; return value; }

constexpr bool expired() {
  const Empty* pointer = nullptr;
  {
    Empty object = make();
    pointer = &object;
  }
  return pointer->value() == 7;
}

static_assert(expired()); //! error
