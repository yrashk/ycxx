// REQUIRES: syntax
// [dcl.array], [expr.sizeof], [temp.dep.constexpr], [temp.inst]
template<class T> struct Bytes {
  unsigned char data[sizeof(T)];
};

Bytes<unsigned char> narrow;
Bytes<int> wide;

static_assert(sizeof(narrow.data) == sizeof(unsigned char));
static_assert(sizeof(wide.data) == sizeof(int));
