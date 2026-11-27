// #340: an `inline static` non-constexpr static data member template: each specialization is
// one object, odr-usable, addressable and writable; different specializations are distinct
// objects ([temp.mem], [class.static.data], [basic.def.odr], [temp.inst]/7).
// EXIT: 0
struct C {
  template <class T> inline static T counter = T(1);
  static int bump() { return ++counter<int>; }
};
template <class A> struct D {
  template <class T> inline static int slot = int(sizeof(A) + sizeof(T));
};
int main() {
  int* p = &C::counter<int>;
  *p = 5;
  if (C::counter<int> != 5) return 1;
  if (C::bump() != 6 || *p != 6) return 2;
  if (C::counter<long> != 1) return 3;
  if ((void*)&C::counter<long> == (void*)&C::counter<int>) return 4;
  if (&D<int>::slot<char> != &D<int>::slot<char>) return 5;
  if (&D<int>::slot<char> == &D<char>::slot<int>) return 6;
  if (D<int>::slot<char> != 5 || D<char>::slot<int> != 5) return 7;
  D<int>::slot<char> = 9;
  if (D<char>::slot<int> != 5 || D<int>::slot<char> != 9) return 8;
  int& r = D<long>::slot<long>;
  r += 1;
  return D<long>::slot<long> == int(2 * sizeof(long)) + 1 ? 0 : 9;
}
