// [expr.throw] A throw-expression has type void and takes an optional assignment-expression.
void rethrow() {
  throw;
}

static_assert(__is_same(decltype(throw), void));
static_assert(__is_same(decltype(throw 42), void));
static_assert(__is_same(decltype(throw true ? 1 : 2), void));
static_assert(__is_same(decltype(throw 1, 42), int));
