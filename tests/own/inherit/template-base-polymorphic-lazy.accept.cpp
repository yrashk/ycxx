// A pointer declaration leaves the dependent base clause uninstantiated ([temp.inst]).
struct Root { virtual int f(); };
template<class Base> struct Derived : Base {};

Derived<Root>* pointer;
