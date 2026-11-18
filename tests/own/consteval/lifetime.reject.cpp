// [expr.const], [basic.indet], [basic.life], [basic.stc.auto]: reading an uninitialized local,
// or reading through a pointer to a local whose lifetime has ended, is not a constant
// expression. Each bad read is on its marked line.
constexpr int assigned_later() {
  int x;
  x = 4;
  return x;
}
static_assert(assigned_later() == 4);
constexpr int uninit() { int x; return x; } static_assert(uninit() == 0); //! error
constexpr int hole() { int a[3]; a[0] = 1; a[2] = 3; return a[1]; } static_assert(hole() == 0); //! error
constexpr int partly(bool b) { int x; if (b) { x = 1; } return x; } static_assert(partly(false)); //! error
constexpr int dangling() { int* p = nullptr; { int x = 1; p = &x; } return *p; } static_assert(dangling()); //! error
constexpr int* escape(int v) { return &v; }
static_assert(*escape(3) == 3); //! error
static_assert(partly(true) == 1);
