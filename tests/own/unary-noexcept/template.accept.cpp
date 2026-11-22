// [expr.unary.noexcept], [except.spec], [temp.inst]
struct Owner { Owner(Owner&&) noexcept {} };
struct RiskyOwner { RiskyOwner(RiskyOwner&&) {} };

template<class T, bool ShouldBeSafe>
void check_move(T&& owner) {
  static_assert(noexcept(T(static_cast<T&&>(owner))) == ShouldBeSafe);
}

void check_instantiations(Owner&& owner, RiskyOwner&& risky) {
  check_move<Owner, true>(static_cast<Owner&&>(owner));
  check_move<RiskyOwner, false>(static_cast<RiskyOwner&&>(risky));
}
