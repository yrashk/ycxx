// Local classes (#117), spec-only: a local class may be a template argument: it is deduced for a
// function template and named explicitly for a class template ([temp.arg.type], [class.local]).
template <class T> T twice_value(T t) {
  t.v = t.v * 2;
  return t;
}

template <class T> struct Wrap {
  T inner;
  int get() const { return inner.v + 1; }
};

template <class T> constexpr int size_of() { return sizeof(T); }

int main() {
  struct Num { int v; };
  Num n{21};
  Num m = twice_value(n);
  if (m.v != 42) return 1;
  Wrap<Num> w{{5}};
  if (w.get() != 6) return 2;
  if (size_of<Num>() != sizeof(int)) return 3;
  struct Other { char c[3]; };
  if (size_of<Other>() != 3) return 4;
  Wrap<Num> arr[2] = {{{1}}, {{2}}};
  if (arr[0].get() + arr[1].get() != 5) return 5;
  return 0;
}
