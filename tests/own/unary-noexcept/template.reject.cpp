// [expr.unary.noexcept], [except.spec], [temp.inst]
struct Owner { Owner(Owner&&) {} };

template<class T>
void require_noexcept_move(T&& owner) {
  static_assert(noexcept(T(static_cast<T&&>(owner))));  //! error
}

void check_instantiation(Owner&& owner) {
  require_noexcept_move<Owner>(static_cast<Owner&&>(owner));
}
