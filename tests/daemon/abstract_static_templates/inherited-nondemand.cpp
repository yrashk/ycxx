struct Base { virtual void f() = 0; };
template<class T> struct A : Base { static A object; };
A<int>* declaration_only;
