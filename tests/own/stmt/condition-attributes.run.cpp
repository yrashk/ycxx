// #337 (ι, review): a declaration in a condition or an init-statement may begin with an
// attribute-specifier-seq ([stmt.pre]/1, [dcl.pre]/5).
// EXIT: 0
int f(int v) {
  if ([[maybe_unused]] int x = v) return 1;
  switch ([[maybe_unused]] int y = v; v) { case 0: break; }
  int s = 0;
  for ([[maybe_unused]] int i = 0; i < 3; ++i) s += i;
  return s == 3 ? 2 : 3;
}
int main() { return f(0) == 2 && f(1) == 1 ? 0 : 1; }
