// ζ f1 (#88): a call whose arguments are not dependent is looked up where the template is
// defined; helper is declared only after the template, so the name is not found, whether or not
// the template is instantiated ([temp.res], [temp.nondep], [basic.lookup.unqual]).
template<class T>
int use(T x) {
  return helper(3) + x; //! error
}

int helper(int v) { return v; }

int main() {
  return use(1);
}
