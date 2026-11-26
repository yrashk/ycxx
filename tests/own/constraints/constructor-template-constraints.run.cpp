// #1398: a constructor template whose associated constraints are not satisfied is not a
// candidate ([temp.deduct.general]/5), in an ordinary class and in a class template whose
// constraint names both template parameters.
// EXIT: 0
struct S {
  int k;
  template <class U> requires (sizeof(U) > 4) S(U) : k(1) {}
  S(char) : k(2) {}
};

template <class T> struct W {
  int k;
  template <class U> requires (sizeof(U) > sizeof(T)) W(U) : k(1) {}
  W(char) : k(2) {}
};

int main() {
  S a(1);       // int is too small: S(char)
  S b(1L);      // the template
  W<char> c(1);  // 4 > 1: the template
  W<long> d(1);  // 4 > 8 fails: W(char)
  if (a.k != 2) return 1;
  if (b.k != 1) return 2;
  if (c.k != 1) return 3;
  if (d.k != 2) return 4;
  return 0;
}
