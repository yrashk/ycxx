// ζ c2 (#88): a class template at namespace scope with one type parameter, a data member of the
// parameter's type and a member function defined in the class; Box<int> and Box<char> are
// specializations initialized as aggregates ([temp.class], [temp.names], [temp.inst],
// [dcl.init.aggr]).
// EXIT: 0
template<class T>
struct Box {
  T v;
  T get() const { return v; }
};

int main() {
  Box<int> a{41};
  if (a.v != 41) return 1;
  if (a.get() != 41) return 2;
  a.v = 7;
  if (a.get() != 7) return 3;
  Box<char> c{'x'};
  if (c.get() != 'x') return 4;
  Box<long> l{-5L};
  if (l.get() != -5L) return 5;
  Box<bool> b{true};
  if (!b.get()) return 6;
  return 0;
}
