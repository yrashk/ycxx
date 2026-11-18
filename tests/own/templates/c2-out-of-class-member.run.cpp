// ζ c2 (#88): member functions of a class template declared in the class and defined outside it
// with template<class T> and the qualifier Box<T>::, as const and non-const members, with a
// non-type parameter, and with the injected-class-name usable after the qualifier
// ([temp.mem.func], [temp.class], [temp.local]).
// EXIT: 0
template<class T>
struct Box {
  T v;
  T get() const;
  void set(T x);
  bool same(const Box& o) const;
  Box* self();
};

template<class T>
T Box<T>::get() const { return v; }

template<class T>
void Box<T>::set(T x) { v = x; }

template<class T>
bool Box<T>::same(const Box& o) const { return v == o.get(); }

template<class T>
Box<T>* Box<T>::self() { return this; }

template<class T, int N>
struct Arr {
  T data[N];
  T sum() const;
};

template<class T, int N>
T Arr<T, N>::sum() const {
  T s = 0;
  for (int i = 0; i < N; ++i) s += data[i];
  return s;
}

int main() {
  Box<int> a{4};
  if (a.get() != 4) return 1;
  a.set(9);
  if (a.get() != 9) return 2;
  Box<int> b{9};
  if (!a.same(b)) return 3;
  b.set(1);
  if (a.same(b)) return 4;
  if (a.self() != &a) return 5;
  Box<char> c{'c'};
  c.set('d');
  if (c.get() != 'd') return 6;
  Arr<int, 3> arr{{1, 2, 3}};
  if (arr.sum() != 6) return 7;
  Arr<long, 2> la{{10L, -4L}};
  if (la.sum() != 6L) return 8;
  return 0;
}
