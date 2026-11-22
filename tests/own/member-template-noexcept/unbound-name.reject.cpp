// [except.spec], [temp.res]: check the written specification before instantiation.
struct S { template<class T> int f() noexcept(no_such_name) { return 0; } };  //! error
int main() {}
