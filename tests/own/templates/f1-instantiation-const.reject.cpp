// ζ f1 (#88): from a const int lvalue, T& deduces T = const int; the instantiated body assigns
// through a reference to const, an error inside the template body ([temp.deduct.call],
// [temp.inst], [expr.assign]).
template<class T>
void twice(T& r) {
  r = r * 2; //! error
}

int main() {
  int i = 1;
  twice(i);
  const int ci = 2;
  twice(ci);
  return 0;
}
