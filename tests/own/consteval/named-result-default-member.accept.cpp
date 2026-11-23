// #975 [dcl.init.aggr], [stmt.return], [expr.const]
struct Earlier { int first; int second = first; };
consteval Earlier make_earlier() { Earlier value{17}; return value; }
static_assert(make_earlier().first == 17 && make_earlier().second == 17);
