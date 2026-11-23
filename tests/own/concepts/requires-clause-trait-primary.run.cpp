// #218: a type-trait built-in is a primary-expression, so it can be a requires-clause's operand
// without parentheses ([temp.pre]/9). Its parenthesized operands are not the declarator's
// parameters: the template that follows is still declared.
template<class T> requires __is_same(T, int) int f(T*) { return 1; }
int f(const void*) { return 2; }

template<class T> requires __is_class(T) && __is_empty(T) constexpr int v = 3;

template<class T> requires __is_same(T, long) struct S { static constexpr int value = 4; };

template<class T>
requires __is_same(T, char) || __is_same(T, signed char)
int g(T) { return 5; }

struct Empty {};

int main() {
  int i = 0;
  long l = 0;
  if (f(&i) != 1) return 1;
  if (f(&l) != 2) return 2;
  if (v<Empty> != 3) return 3;
  if (S<long>::value != 4) return 4;
  return g('x') == 5 ? 0 : 6;
}
