struct Quiet { ~Quiet() noexcept {} };
struct Loud { ~Loud() noexcept(false) {} };
struct Defaulted {};
void inspect(Quiet& quiet, Loud& loud, Defaulted& trivial) {
  static_assert(noexcept(quiet.~Quiet()));
  static_assert(!noexcept(loud.~Loud()));
  static_assert(noexcept(trivial.~Defaulted()));
}
