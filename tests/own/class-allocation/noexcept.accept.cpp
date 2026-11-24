// [expr.unary.noexcept], [expr.new], [expr.delete]: a class new-expression can throw through
// its allocation function and constructor; a delete-expression through its destructor.
struct Plain {
  int v;
};
struct Quiet {
  Quiet() noexcept;
  ~Quiet();
};
struct Loud {
  ~Loud() noexcept(false);
};
Plain* plain;
Quiet* quiet;
Loud* loud;
static_assert(!noexcept(new Plain));
static_assert(!noexcept(new Quiet()));
static_assert(noexcept(delete plain));
static_assert(noexcept(delete quiet));
static_assert(!noexcept(delete loud));
int main() {}
