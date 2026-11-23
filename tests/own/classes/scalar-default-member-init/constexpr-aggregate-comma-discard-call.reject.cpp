// [dcl.init.aggr], [expr.context], [expr.comma], [basic.life], [expr.const]
constexpr int id(int x) { return x; }

struct R {
  int earlier;
  int selected = earlier ? earlier : (id(later), earlier);
  int later = 2;
};

constexpr R skipped{1};
static_assert(skipped.selected == 1);

struct Initialized {
  int earlier;
  int selected = earlier ? earlier : (id(earlier), earlier);
};

constexpr Initialized chosen{0};
static_assert(chosen.selected == 0);

constexpr R bad{0}; //! error
