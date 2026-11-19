// ι (#291), from the review: a constexpr function may define a static variable (C++23); a
// constant evaluation that does not pass its definition is constant ([dcl.constexpr], [expr.const]).
// EXIT: 0
constexpr int f(bool b) {
  if (b) {
    static int x = 0;
    return ++x;
  }
  return 1;
}
static_assert(f(false) == 1);
int main() { return f(true) + f(true) == 3 ? 0 : 1; }
