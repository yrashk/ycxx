// ζ c2 (#88): constructors of a class template, defined in the class with the injected-class-name
// and outside it as Box<T>::Box, with mem-initializers of dependent type, overloaded on the
// number of parameters, and a default constructor; locals are constructed directly
// ([temp.class], [temp.local], [class.ctor], [class.base.init]).
// EXIT: 0
template<class T>
struct Box {
  T v;
  int which;
  Box() : v(0), which(1) {}
  Box(T x) : v(x), which(2) {}
  Box(T x, T y);
};

template<class T>
Box<T>::Box(T x, T y) : v(x + y), which(3) {}

template<class T, int N>
struct Filled {
  T data[N];
  Filled(T x) {
    for (int i = 0; i < N; ++i) data[i] = x;
  }
  T at(int i) const { return data[i]; }
};

int main() {
  Box<int> a;
  if (a.v != 0 || a.which != 1) return 1;
  Box<int> b(5);
  if (b.v != 5 || b.which != 2) return 2;
  Box<int> c(2, 3);
  if (c.v != 5 || c.which != 3) return 3;
  Box<long> d{7L};
  if (d.v != 7L || d.which != 2) return 4;
  Box<char> e('a', 1);
  if (e.v != 'b' || e.which != 3) return 5;
  Filled<int, 4> f(6);
  if (f.at(0) != 6 || f.at(3) != 6) return 6;
  return 0;
}
