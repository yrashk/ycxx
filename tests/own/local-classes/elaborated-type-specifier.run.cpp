// Local classes (#117), spec-only: an elaborated-type-specifier `struct Later` in a block declaration,
// when lookup finds nothing, declares a new class in that block; it is completed later in the same
// block, and another block can declare its own `Later` ([dcl.type.elab], [basic.scope.pdecl],
// [class.local]).
int first() {
  struct Later* p = nullptr;
  if (p != nullptr) return -1;
  struct Later { int v; };
  Later obj{7};
  p = &obj;
  return p->v;
}

int second() {
  struct Later* p = nullptr;
  struct Later {
    long a;
    long b;
  } obj{3, 4};
  p = &obj;
  if (sizeof(Later) != 2 * sizeof(long)) return -1;
  return static_cast<int>(p->a + p->b);
}

int main() {
  if (first() != 7) return 1;
  if (second() != 7) return 2;
  return 0;
}
