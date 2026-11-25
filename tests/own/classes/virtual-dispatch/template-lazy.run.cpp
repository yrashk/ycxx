// #1153 [temp.inst]: the virtual members a table needs are instantiated where it is used, with
// what they use in turn, and complete types named before their definition. A table that nothing
// installs needs none.
// EXIT: 0

template <class T> struct Broken {
  virtual ~Broken() {}
  virtual int f() { return T::missing(); }
};
struct Unused : Broken<int> {
  int f() override { return 1; }
};
extern Unused& never;

template <class T> struct Helper {
  int go() { return 3; }
};
template <class T> struct Calls {
  virtual int f() {
    Helper<T> h;
    return h.go() + k();
  }
  virtual int k() { return 1; }
};
template <class T> struct Inherits : Calls<T> {};

struct Later;
template <class T> struct Uses {
  virtual int f() {
    T t;
    return t.x;
  }
  T* p = nullptr;
};
template <class T> struct Holds {
  virtual int f() { return t.x; }
  T t;
};
struct Later {
  int x = 4;
};

template <class T> struct Pure {
  virtual ~Pure() = default;
  virtual int f() = 0;
};
template <class T> int Pure<T>::f() { return 9; }
struct Final : Pure<int> {
  int f() override { return Pure<int>::f() + 1; }
};

Inherits<short> global;

int main() {
  Calls<short>& c = global;
  if (c.f() != 4) return 1;
  Uses<Later> u;
  Uses<Later>& ru = u;
  Holds<Later> h;
  Holds<Later> copy = h;
  Holds<Later>& rh = copy;
  if (ru.f() + rh.f() != 8) return 2;
  Final f;
  Pure<int>& p = f;
  return p.f() == 10 ? 0 : 3;
}
