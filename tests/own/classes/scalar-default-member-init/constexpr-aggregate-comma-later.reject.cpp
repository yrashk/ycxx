// [dcl.init.aggr], [expr.cond], [expr.comma], [basic.life], [expr.const]
struct R {
  int earlier = 0;
  int selected = earlier ? earlier : (0, later);
  int later = 2;
};

constexpr R r{}; //! error
