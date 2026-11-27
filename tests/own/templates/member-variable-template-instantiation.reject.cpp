// #340: a static data member template's initializer is instantiated only for a specialization
// that is used, so a hard error appears in the initializer only when `bad<int>` is named; an
// unused bad specialization is no error ([temp.inst]/7, [temp.mem]).
struct C {
  template <class T> static constexpr int bad = T::missing;  //! error
  template <class T> static constexpr int unused = T::missing;
};
template <class A> struct D {
  template <class T> static constexpr int bad = A::missing + int(sizeof(T));  //! error
};
struct Has { static constexpr int missing = 4; };
static_assert(C::bad<Has> == 4);
static_assert(D<Has>::bad<char> == 5);
int a = C::bad<int>;
int b = D<long>::bad<char>;
int main() { return 0; }
