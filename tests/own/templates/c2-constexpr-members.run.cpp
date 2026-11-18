// ζ c2 (#88): constexpr constructors and member functions of a class template are usable in
// constant expressions (static_assert, an array bound, a template argument), and the same
// calls work at run time ([temp.class], [dcl.constexpr], [expr.const], [temp.arg.nontype]).
// EXIT: 0
template<class T>
struct Box {
  T v;
  constexpr Box(T x) : v(x) {}
  constexpr T get() const { return v; }
  constexpr T twice() const { return get() + get(); }
};

template<int N>
struct Arr {
  int data[N];
  constexpr int size() const { return N; }
};

static_assert(Box<int>(4).get() == 4);
static_assert(Box<long>(5L).twice() == 10L);
static_assert(Arr<3>{}.size() == 3);

constexpr Box<int> six(6);
int buffer[six.twice()];
Arr<six.get()> sized{};

int main() {
  if (sized.size() != 6) return 1;
  int n = 7;
  Box<int> runtime(n);
  if (runtime.twice() != 14) return 2;
  if (six.get() != 6) return 3;
  buffer[11] = 1;
  if (buffer[11] != 1) return 4;
  return 0;
}
