// #102, [expr.const]: libycxx's comparison-category argument
// (deps/libycxx/include/ycxx/core/compare.hpp). Converting a nonzero literal calls a function
// that is not constexpr, so the immediate invocation is not a constant expression.
// XFAIL: gcc reports the call inside the constructor's body as a second error
void bad();
struct literal_zero {
  consteval literal_zero(int v) noexcept {
    if (v != 0) bad();
  }
};
void f(literal_zero);
void call_with_one() {
  f(1); //! error
}
struct ordering {
  int value;
};
bool operator==(ordering, literal_zero);
bool compare(ordering x) { return x == 1; } //! error
bool compare_zero(ordering x) { return x == 0; }
