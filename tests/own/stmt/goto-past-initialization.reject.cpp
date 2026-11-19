// #337 (ι): a jump forward past a declaration with non-vacuous initialization into the scope of
// the variable is ill-formed ([stmt.dcl]/2). The goto, the declaration and the label are on one
// line because compilers report it at the goto or at the label.
int f() {
  goto l; int x = 1; l: return x; //! error
}

int main() { return f(); }
