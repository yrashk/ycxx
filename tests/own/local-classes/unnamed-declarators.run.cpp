// Local classes (#117), spec-only: an unnamed class defined in a block declares objects through the
// declarators that follow its class-specifier, including a const array with an aggregate
// initializer whose bound is deduced ([class.local], [class.pre], [dcl.init.aggr]).
int main() {
  struct { int a; } x{1};
  if (x.a != 1) return 1;
  const struct {
    int e;
    int v;
  } table[] = {{1, 10}, {2, 20}, {3, 30}};
  if (sizeof(table) / sizeof(table[0]) != 3) return 2;
  int found = 0;
  for (auto const& row : table)
    if (row.e == 2) found = row.v;
  if (found != 20) return 3;
  struct { int lo, hi; } r{4, 9}, *pr = &r;
  if (pr->hi - pr->lo != 5) return 4;
  return 0;
}
