// #157 [expr.prim.paren], [expr.call], [expr.unary.noexcept], [except.spec].
struct ParenthesizedCall {
  static void configure(int = 0) noexcept;
  ~ParenthesizedCall() noexcept(noexcept((configure)())) = default;
};

static_assert(noexcept(static_cast<ParenthesizedCall*>(nullptr)->~ParenthesizedCall()));
