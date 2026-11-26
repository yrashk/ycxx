// REQUIRES: syntax
// [dcl.array], [expr.sizeof], [temp.dep.constexpr]
template<unsigned N> struct Direct {
  unsigned char data[N];
};

template<class T> struct Fixed {
  unsigned char data[sizeof(int)];
};

struct Incomplete;
constexpr Direct<3> direct{{2, 3, 5}};
constexpr Fixed<Incomplete> fixed{{7}};

static_assert(sizeof(direct.data) == 3 && direct.data[2] == 5);
static_assert(sizeof(fixed.data) == sizeof(int) && fixed.data[0] == 7);
