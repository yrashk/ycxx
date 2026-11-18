// δ s1 (#86): a local class, struct locals in loops, a const struct, a global aggregate with an
// initializer, members of unsigned, long, char and bool type ([class.local], [dcl.init.aggr],
// [basic.start.static]).
// EXIT: 0
struct Stats { unsigned count; long total; char tag; bool ok; };

Stats global = {2, 30, 'g', true};

int main() {
  struct Local { int lo; int hi; };
  Local ranges[3] = {{0, 4}, {10, 13}, {20, 21}};
  int len = 0;
  for (int i = 0; i < 3; i++) {
    Local r = ranges[i];
    len += r.hi - r.lo;
  }
  if (len != 8) return 1;
  const Stats k = {1, 5, 'k', false};
  if (k.total != 5 || k.ok) return 2;
  if (global.count != 2u || global.tag != 'g' || !global.ok) return 3;
  global.total += k.total;
  if (global.total != 35) return 4;
  Stats s = global;
  s.count = s.count - 3;  // unsigned arithmetic wraps ([basic.fundamental])
  if (s.count != 4294967295u) return 5;
  return 0;
}
