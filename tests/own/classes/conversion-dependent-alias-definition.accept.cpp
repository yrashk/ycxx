// #339: an in-class conversion function can name a dependent member type
// alias as its conversion-type-id ([class.conv.fct]).
template<class T, T V>
struct Constant {
  using value_type = T;
  constexpr operator value_type() const noexcept { return V; }
};
