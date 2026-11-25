// #1153 [class.cdtor]: a constructor template's specialization installs its class's table, in a
// class template and in an ordinary class. A default member initializer names a function template.
// EXIT: 0

template <class T> T next(T x) { return x + 1; }

template <class T> struct S {
  template <class U> S(U u) : v(T(u)) {}
  virtual ~S() {}
  virtual int f() { return int(v); }
  T v;
};
struct Plain {
  template <class U> Plain(U u) : v(int(u)) {}
  virtual int f() { return v; }
  int v;
};
template <class T> struct Initialized {
  virtual int f() { return int(x); }
  T x = next(T(3));
};

int main() {
  S<int> s(4);
  S<int>& r = s;
  Plain p('a');
  Plain& q = p;
  Initialized<long> i;
  Initialized<long>& j = i;
  S<int> local([] { return 5; }());
  S<int>& k = local;
  return r.f() == 4 && q.f() == 'a' && j.f() == 4 && k.f() == 5 ? 0 : 1;
}
