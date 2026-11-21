// The four substituted bool pairs select four distinct overloads (#209).
// EXIT: 0
template<bool, bool> struct Tag {};

template<class T> using Signature = Tag<__is_const(T), __is_volatile(T)>;

int select(Signature<const volatile int*>) { return 1; }
int select(Signature<int* const>) { return 2; }
int select(Signature<const int* volatile>) { return 3; }
int select(Signature<const volatile int>) { return 4; }

int main() {
  if (select(Tag<false, false>{}) != 1)
    return 1;
  if (select(Tag<true, false>{}) != 2)
    return 2;
  if (select(Tag<false, true>{}) != 3)
    return 3;
  if (select(Tag<true, true>{}) != 4)
    return 4;
  return 0;
}
