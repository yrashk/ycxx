// ζ c2 (#88): inside a member function of a class template, a call whose arguments are not
// dependent is bound where the template is defined: helper(1) uses helper(long), the only
// declaration then visible, although helper(int), a better match, is declared before the
// instantiation. A dependent call with an int argument also sees only helper(long): ordinary
// lookup happens at the definition, and argument-dependent lookup adds nothing for a fundamental
// type ([temp.res], [temp.nondep], [temp.dep.candidate], [basic.lookup.argdep]).
// EXIT: 0
int helper(long) { return 1; }

template<class T>
struct Box {
  T v;
  int fixed() const { return helper(1); }
  int dependent() const { return helper(v); }
};

int helper(int) { return 2; }

int main() {
  Box<int> b{0};
  if (b.fixed() != 1) return 1;
  if (b.dependent() != 1) return 2;
  if (helper(1) != 2) return 3;
  return 0;
}
