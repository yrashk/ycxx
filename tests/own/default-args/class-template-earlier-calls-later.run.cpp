// #356: a member function defined earlier in a class template calls a member declared later
// and relies on its dependent default argument. The body is a complete-class context, and the
// default is instantiated at the call ([class.mem.general], [temp.inst], [dcl.fct.default]).
// EXIT: 0
struct Traits { static constexpr int eof() { return -1; } };
struct Other { static constexpr int eof() { return 7; } };

template<class T>
struct Buffer {
  int unget() { return pb(); }
  int unget_with(int c) { return pb(c); }
  int twice() { return pb() + pb(); }
  virtual int pb(int c = T::eof()) { return c; }
  virtual ~Buffer() = default;
};

template<class T>
struct Logged : Buffer<T> {
  int seen = 0;
  int pb(int c) override { seen = c; return c * 10; }
};

int main() {
  Buffer<Traits> b;
  if (b.unget() != -1) return 1;
  if (b.unget_with(5) != 5) return 2;
  Buffer<Other> o;
  if (o.twice() != 14) return 3;
  Logged<Other> l;
  if (l.unget() != 70 || l.seen != 7) return 4;
  return 0;
}
