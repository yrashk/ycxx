// Local classes (#117), spec-only: a local class name is scoped to its block; it is not visible
// after the block ends, nor in another function ([basic.scope.block], [class.local]).
int f() {
  {
    struct Inner { int v; };
    Inner i{1};
  }
  Inner j{2}; //! error
  return 0;
}

int g() {
  Inner k{3}; //! error
  return 0;
}

int main() { return f() + g(); }
