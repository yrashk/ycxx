// #1065 GNU __builtin_constant_p takes an expression and returns an int prvalue.
static_assert(__is_same(decltype((__builtin_constant_p(7))), int));
static_assert(__builtin_constant_p(7) == 1);
static_assert(__builtin_constant_p(4 * (8 - 3)) == 1);
static_assert(__builtin_constant_p(4 < 3) == 1);

extern int runtime_value;
int may_throw();
int cannot_throw() noexcept;

static_assert(__builtin_constant_p(runtime_value) == 0);
static_assert(__builtin_constant_p(may_throw()) == 0);
static_assert(!noexcept(__builtin_constant_p(may_throw())));
static_assert(noexcept(__builtin_constant_p(cannot_throw())));

template<int Value>
constexpr int constant_after_substitution() {
  return __builtin_constant_p(Value + 1);
}

static_assert(constant_after_substitution<-1>() == 1);
static_assert(constant_after_substitution<17>() == 1);
