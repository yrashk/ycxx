struct Base { virtual void f() = 0; };
template<class T> struct A : Base { static A object; };
static_assert(__is_abstract(A<int>));
