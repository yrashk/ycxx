// [dcl.init.aggr], [class.mem.general], [expr.const]
struct R {
  unsigned x = 1;
  unsigned y = x;
};

constexpr R r{};
static_assert(r.y == 1);

struct Config {
  static constexpr R r = R{};
};
static_assert(Config::r.y == 1);
