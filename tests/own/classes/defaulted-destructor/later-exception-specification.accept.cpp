// #157 [class.mem.general], [class.member.lookup], [except.spec], [expr.unary.noexcept].
struct LaterExceptionSpec {
  ~LaterExceptionSpec() noexcept(flag) = default;
  static constexpr bool flag = false;
};

void check_exception_spec(LaterExceptionSpec& object) {
  static_assert(!noexcept(object.~LaterExceptionSpec()));
}
