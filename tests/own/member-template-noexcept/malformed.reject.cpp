// [except.spec], [temp.res]: check the written specification before instantiation.
struct S { template<class T> int f() noexcept(+) { return 0; } };  //! error
int main() {}
