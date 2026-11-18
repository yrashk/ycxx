// Unqualified name lookup in block and namespace scope ([basic.lookup.unqual]): the innermost
// declaration wins, a function's name is visible in its own body ([basic.scope.pdecl]), and a
// name declared later in the file is not visible before it.
int value = 1;
int recurse(int n) { return n == 0 ? 0 : recurse(n - 1) + value; }
int shadow(int value) {
  {
    int value2 = value;
    {
      int value = value2 + 1;
      return value;
    }
  }
}
int main() {
  int value = 2;
  for (int value = 3; value < 4; ++value) {
  }
  return recurse(value) + shadow(value);
}
