// The peer of member-variable-template-abi.run.cpp: the same classes, and uses of the
// specializations the test also instantiates. The oracle's definitions are weak; the linker
// resolves both units' uses to one object.
template <class C> struct S {
  template <class T> inline static int counter = sizeof(T) + sizeof(C);
};
struct N {
  template <class T> inline static long k = 7;
  template <class T> static const int w;
};
template <class T> const int N::w = sizeof(T) * 2;
namespace ns {
template <int I> struct Q {
  template <class T> static constexpr long z = I;
};
}
int peer_counter() { return S<int>::counter<char>; }
void peer_bump() {
  ++S<int>::counter<char>;
  ++N::k<short>;
}
long peer_q() { return *&ns::Q<3>::z<S<int>>; }
int peer_w() { return *&N::w<short>; }
