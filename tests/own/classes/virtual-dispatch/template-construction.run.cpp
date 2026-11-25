// #1153 [class.cdtor], [class.base.init]: the table in force during construction and destruction
// of class template specializations, their implicit special members, and members of their type.
// EXIT: 0

int order = 0;
void note(int d) { order = order * 10 + d; }

template <class T> struct First {
  First() { note(kind()); }
  virtual ~First() { note(kind()); }
  virtual int kind() { return 1; }
};

template <class T> struct Second : First<T> {
  Second() { note(this->kind()); }
  ~Second() { note(this->kind()); }
  int kind() override { return 2; }
};

template <class T> struct Value {
  int a = 4;
  T b{};
  virtual int sum() { return a + int(b); }
};

struct Plain : Value<int> {
  int sum() override { return 100 + Value<int>::sum(); }
};

template <class T> struct Wrap {
  Value<T> inner;
  int n;
};

int sum(Value<int>& v) { return v.sum(); }

int main() {
  { Second<char> s; }
  if (order != 1221) return 1;
  Plain p;
  p.b = 3;
  if (sum(p) != 107) return 2;
  Value<int> sliced = p;
  if (sum(sliced) != 7) return 3;
  Plain q = p;
  if (sum(q) != 107) return 4;
  Wrap<int> w{};
  w.inner.b = 5;
  Wrap<int> copy = w;
  return sum(copy.inner) == 9 && copy.n == 0 ? 0 : 5;
}
