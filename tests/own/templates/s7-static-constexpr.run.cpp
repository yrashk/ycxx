// ζ s7 (#88): a class template's static constexpr data members, of dependent type or with a
// value-dependent initializer, have their value in each specialization and are usable in constant
// expressions; one that is odr-used has a definition ([class.static.data]/4, [temp.inst]/3).
// EXIT: 0
template<class T> struct V { static constexpr T one = T(1); static constexpr T two = one + one; };
template<class T> struct W { static constexpr int n = sizeof(T); int get() const { return n; } static int sget() { return n * 2; } };
template<class T> struct K { static const int k = 7; static constexpr bool b = V<T>::two == 2; };
template<class T> struct I { static constexpr int width = 0; };
template<> struct I<int> { static constexpr int width = 32; };
template<class T> int width() { return I<T>::width; }
static_assert(V<char>::two == 2 && W<short>::n == 2 && I<char>::width == 0 && I<int>::width == 32);
int main() {
  W<int> w;
  const int& r = W<long>::n;
  const int* p = &V<int>::two;
  if (V<char>::two != 2 || V<long>::one != 1) return 1;
  if (w.get() != 4 || W<short>::sget() != 4) return 2;
  if (r != 8 || *p != 2) return 3;
  if (K<int>::k != 7 || !K<int>::b) return 4;
  return width<int>() == 32 && width<long>() == 0 ? 0 : 5;
}
