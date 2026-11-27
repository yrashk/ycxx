// REQUIRES: syntax
// [dcl.array], [expr.sizeof], [temp.dep.constexpr]
template<unsigned N> struct Direct {
  unsigned char data[N];
};

template<class T> struct Fixed {
  unsigned char data[sizeof(int)];
};

struct Incomplete;
Direct<3> direct;
Fixed<Incomplete> fixed;

static_assert(sizeof(direct.data) == 3);
static_assert(sizeof(fixed.data) == sizeof(int));
