// #337 (ι): `goto` needs an identifier ([stmt.jump.general]); a `case` label needs its `:`
// ([stmt.label]).
int f(int x) {
  goto; //! error
}
int g(int x) {
  switch (x) {
  case 1 return 1; //! error
  }
  return 0;
}
int main() { return f(0) + g(0); }
