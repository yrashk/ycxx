// δ s1 (#86): default member initializers, used when an aggregate initializer omits the member
// and for default-initialized objects; one may use an earlier member ([class.mem.general],
// [dcl.init.aggr], [class.base.init]).
// EXIT: 0
struct Settings {
  int level = 5;
  int retries;
  int limit = level * 2;
  bool on = true;
};

int main() {
  Settings a = {1, 2};
  if (a.level != 1 || a.retries != 2 || a.limit != 2 || !a.on) return 1;
  Settings b{};
  if (b.level != 5 || b.retries != 0 || b.limit != 10 || !b.on) return 2;
  Settings c = {7, 1, 3, false};
  if (c.limit != 3 || c.on) return 3;
  Settings d;  // default-initialized: the default member initializers still apply
  if (d.level != 5 || d.limit != 10 || !d.on) return 4;
  d.retries = 4;
  if (d.retries != 4) return 5;
  return 0;
}
