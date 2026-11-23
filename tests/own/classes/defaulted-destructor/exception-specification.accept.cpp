// #157 [class.dtor], [dcl.fct.def.default], [except.spec], [class.prop].
struct DefaultExceptionSpec {
  int value;
  ~DefaultExceptionSpec() = default;
};

struct ExplicitExceptionSpec {
  int value;
  ~ExplicitExceptionSpec() noexcept(false) = default;
};

static_assert(__is_trivially_copyable(DefaultExceptionSpec));
static_assert(__is_trivially_copyable(ExplicitExceptionSpec));

void check_exception_spec(DefaultExceptionSpec& usual, ExplicitExceptionSpec& relaxed) {
  static_assert(noexcept(usual.~DefaultExceptionSpec()));
  static_assert(!noexcept(relaxed.~ExplicitExceptionSpec()));
}
