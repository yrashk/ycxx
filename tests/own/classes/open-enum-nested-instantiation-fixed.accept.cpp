// [dcl.enum], [expr.sizeof], [temp.inst]
template<int N> struct Other { enum Inner { first = 7 }; };
enum Outer : unsigned char { first = Other<7>::first, bytes = sizeof(Outer) };
static_assert(Outer::first == 7);
static_assert(Outer::bytes == sizeof(unsigned char));
