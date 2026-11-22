// [expr.unary.noexcept], [except.spec]
int safe_argument() noexcept;
int risky_argument();
int receive(int) noexcept;
int always_risky(int);

static_assert(noexcept(receive(1)));
static_assert(!noexcept(always_risky(1)));
static_assert(noexcept(receive(safe_argument())));
static_assert(!noexcept(receive(risky_argument())));

void check_unevaluated(int& runtime_value) {
  static_assert(noexcept(receive(runtime_value)));
}
