// #1153 [class.virtual]: a class template whose bases are polymorphic: an ordinary class, a
// dependent specialization, and a template parameter. A member overrides without `virtual`.
// EXIT: 0

struct f {
  virtual ~f() {}
};
template <class C> struct h : f {};

struct Root {
  virtual ~Root() {}
  virtual int id() { return 1; }
  virtual int size() = 0;
};

template <class T> struct Sized : Root {
  int size() override { return int(sizeof(T)); }
  T value{};
};

template <class T> struct Twice : Sized<T> {
  int id() { return 2; }
  int size() override { return 2 * Sized<T>::size(); }
};

template <class Base> struct Mixin : Base {
  int id() override { return 10 + Base::id(); }
  virtual int extra() { return this->id() + this->size(); }
};

int id(Root& r) { return r.id(); }
int size(Root& r) { return r.size(); }

int main() {
  h<char> x;
  f& base = x;
  (void)base;
  Sized<short> s;
  if (id(s) != 1 || size(s) != 2 || s.value != 0) return 1;
  Twice<int> t;
  if (id(t) != 2 || size(t) != 8) return 2;
  Mixin<Twice<char>> m;
  if (id(m) != 12 || size(m) != 2 || m.extra() != 14) return 3;
  Mixin<Mixin<Sized<int>>> mm;
  return id(mm) == 21 && mm.extra() == 25 ? 0 : 4;
}
