// ι (#291): a static block variable is a definition, and a block variable cannot be declared
// twice in one block ([basic.scope.block], [basic.def.odr]); the same name in a nested
// block is fine.
int h() {
  static int n = 0;
  {
    static int n = 1;
    (void)n;
  }
  static int n = 2; //! error
  return n;
}

int main() { return h(); }
