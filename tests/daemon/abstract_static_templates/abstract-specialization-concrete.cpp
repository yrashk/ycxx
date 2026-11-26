struct P { virtual void f() {} };
template<class T> struct A : T { static A object; };
static_assert(!__is_abstract(A<P>));
