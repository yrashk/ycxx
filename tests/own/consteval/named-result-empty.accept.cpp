struct Empty { constexpr unsigned value() const { return 7; } };
consteval Empty make() { Empty value{}; return value; }
static_assert(make().value() == 7);

consteval int scalar_result(int input) {
  int value = input + 7;
  return value;
}
static_assert(scalar_result(35) == 42);
