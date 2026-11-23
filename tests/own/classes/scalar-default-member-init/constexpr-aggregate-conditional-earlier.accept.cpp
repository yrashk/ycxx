// [dcl.init.aggr], [expr.cond], [basic.life], [expr.const]
struct Packet {
  int earlier;
  int selected = earlier != 0 ? earlier : later;
  int later = 91;
};

constexpr Packet positive{37};
static_assert(positive.selected == 37);
