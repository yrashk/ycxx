// Local classes (#117), spec-only: a named local class whose class-specifier is followed by
// declarators declares the class and the objects at once; the name is usable afterwards in the
// block ([class.local], [class.pre], [dcl.dcl]).
int main() {
  struct G {
    int a;
    int b;
  } g{1, 2}, h{3, 4};
  if (g.a + g.b != 3) return 1;
  if (h.a * h.b != 12) return 2;
  G k = h;
  k.a = 10;
  if (k.a + k.b != 14 || h.a != 3) return 3;
  struct G2 { int v; } arr[2]{{7}, {8}}, *p = &arr[1];
  if (p->v != 8 || arr[0].v != 7) return 4;
  return 0;
}
