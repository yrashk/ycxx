// ζ s7 (#88), from the step's review: a static const member of dependent type may have an in-class initializer when its
// specialization's type is integral ([class.static.data]/4).
// EXIT: 0
template<class T> struct S { static const T k = 5; };
int main() { return S<int>::k == 5 ? 0 : 1; }
