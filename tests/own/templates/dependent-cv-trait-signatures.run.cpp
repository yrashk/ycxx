// The four substituted bool pairs select four distinct overloads (#209).
// EXIT: 0
template<bool, bool> struct Tag {};

template<class T> using Signature = Tag<__is_const(T), __is_volatile(T)>;

int select(Signature<const volatile int*>*) { return 1; }
int select(Signature<int* const>*) { return 2; }
int select(Signature<const int* volatile>*) { return 3; }
int select(Signature<const volatile int>*) { return 4; }

int main() {
  Tag<false, false>* unqualified = nullptr;
  Tag<true, false>* const_only = nullptr;
  Tag<false, true>* volatile_only = nullptr;
  Tag<true, true>* const_volatile = nullptr;

  if (select(unqualified) != 1)
    return 1;
  if (select(const_only) != 2)
    return 2;
  if (select(volatile_only) != 3)
    return 3;
  if (select(const_volatile) != 4)
    return 4;
  return 0;
}
