// #340: a member template may declare a static data member but not a non-static one
// ([temp.pre]/2, [temp.mem]).
struct C {
  template <class T> T bad = T(0);  //! error
  template <class T> static constexpr T ok = T(0);
};
template <class A> struct D {
  template <class T> int bad;  //! error
};
int main() { return C::ok<int>; }
