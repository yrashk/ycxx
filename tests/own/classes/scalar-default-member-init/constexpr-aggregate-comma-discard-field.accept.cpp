// [dcl.init.aggr], [expr.context], [expr.comma], [basic.life], [expr.const]
struct R {
  int earlier;
  int selected = earlier ? earlier : (later, earlier);
  int later = 2;
};

constexpr R untaken{1};
constexpr R taken{0};
static_assert(untaken.selected == 1 && taken.selected == 0);
