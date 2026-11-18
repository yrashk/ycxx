// [dcl.constexpr], [dcl.spec]: consteval applies only to functions; a constexpr variable needs
// an initializer; constexpr and consteval cannot both appear in one declaration.
consteval int v = 1; //! error
constexpr int w; //! error
constexpr consteval int f() { return 1; } //! error
consteval constexpr int g() { return 2; } //! error
constexpr int ok = 3;
consteval int h() { return ok; }
void local() {
  constexpr int x; //! error
  consteval int y = 2; //! error
  constexpr int z = 4;
}
