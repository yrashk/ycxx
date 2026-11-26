// REQUIRES: syntax
// [dcl.array], [expr.sizeof], [temp.dep.constexpr], [temp.inst]
template<class T> struct Bytes {
  unsigned char data[sizeof(T)];
};

constexpr Bytes<unsigned char> narrow{{7}};
constexpr Bytes<int> wide{{11}};

static_assert(sizeof(narrow.data) == sizeof(unsigned char));
static_assert(sizeof(wide.data) == sizeof(int));
static_assert(narrow.data[0] == 7 && wide.data[0] == 11);
