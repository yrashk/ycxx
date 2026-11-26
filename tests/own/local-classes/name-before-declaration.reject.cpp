// Local classes (#117), spec-only: a local class name is not visible before its point of
// declaration in the block ([basic.scope.pdecl], [class.local]).
int main() {
  Early* p = nullptr; //! error
  struct Early { int v; };
  return 0;
}
