// ζ c2 (#88): in a member function of a class template, a call whose arguments are not dependent
// is looked up at the definition; helper is declared only after the class template, so the
// call is ill-formed even though the class is never instantiated ([temp.res], [temp.nondep],
// [basic.lookup.unqual]).
template<class T>
struct Box {
  T v;
  int get() const { return helper(1); } //! error
};

int helper(int x) { return x; }
