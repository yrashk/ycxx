// [basic.lookup.unqual], [basic.scope.pdecl]: a name must be declared before it is used, and a
// block-scope name is gone after its block.
int use_before() {
  return later; //! error
}
int later = 1;
int main() {
  {
    int inner = 1;
  }
  int a = inner; //! error
  return a + nowhere(1); //! error
}
