// An unqualified name must be found by lookup ([basic.lookup.unqual]).
//! error@+2:10
int f() {
  return undeclared;
}
