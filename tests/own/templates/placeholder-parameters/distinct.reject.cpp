// #1244: `cw<1>` and `cw<1u>` are different types ([temp.type]/2), so neither converts to the
// other; and `deduced<1>` cannot be explicitly specialized twice ([temp.expl.spec]).
template<auto X> struct cw {};
template<auto N> constexpr unsigned deduced = 0;
template<> constexpr unsigned deduced<1> = 20;
template<> constexpr unsigned deduced<1u> = 21;
template<> constexpr unsigned deduced<1> = 22; //! error
int main() {
  cw<1> a = cw<1u>{}; //! error
  return 0;
}
