// ζ c2 (#88): member functions of a class template call function templates, deducing from a
// member of dependent type, with an explicit argument that is the class's own parameter, and
// with *this, which deduces T from the class template itself (Box<T> declared before the
// function template and defined after it) ([temp.class], [temp.deduct.call],
// [temp.arg.explicit], [temp.dep.candidate]).
// EXIT: 0
template<class T>
struct Box;

template<class T>
T twice(T x) { return x + x; }

template<class R, class T>
R convert(T x) { return x; }

template<class T>
T peek(const Box<T>& b) { return b.v; }

template<int N>
int scale(int x) { return x * N; }

template<class T>
struct Box {
  T v;
  T doubled() const { return twice(v); }
  T doubled_explicit() const { return twice<T>(v); }
  long widened() const { return convert<long>(v); }
  T self_peek() const { return peek(*this); }
  int scaled() const { return scale<3>(v); }
};

template<class T, int N>
struct Arr {
  T data[N];
  int total() const { return scale<N>(1); }
};

int main() {
  Box<int> a{5};
  if (a.doubled() != 10) return 1;
  if (a.doubled_explicit() != 10) return 2;
  if (a.widened() != 5L) return 3;
  if (a.self_peek() != 5) return 4;
  if (a.scaled() != 15) return 5;
  Box<long> l{-2L};
  if (l.doubled() != -4L) return 6;
  if (l.self_peek() != -2L) return 7;
  Arr<char, 4> arr{};
  if (arr.total() != 4) return 8;
  return 0;
}
