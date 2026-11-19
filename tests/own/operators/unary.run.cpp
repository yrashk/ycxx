// #133, from the step's review: unary `*` and `&` call operator functions when an operand has
// class or enumeration type ([over.unary]).
// EXIT: 0
struct S {
  int v;
  int operator&() const { return 42; }
  int operator*() const { return v * 2; }
};
enum E { a = 3 };
int operator*(E e) { return int(e) + 4; }
int main() {
  S s{5};
  return (&s) - 42 + (*s) - 10 + (*a) - 7;
}
