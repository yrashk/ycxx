// ζ c2 (#88): data members whose types depend on the template parameters: T, T*, T[N], a
// reference T&, a specialization of another class template Box<T>, and members of two
// independent parameters; their types are checked through overloads on pointers
// ([temp.class], [temp.dep.type], [class.mem]).
// EXIT: 0
int id(int*) { return 1; }
int id(long*) { return 2; }
int id(char*) { return 3; }
int id(int**) { return 4; }
int id(long**) { return 5; }

template<class T>
struct Box {
  T v;
};

template<class T, class U, int N>
struct Mixed {
  T value;
  T* ptr;
  U items[N];
  Box<T> boxed;
  int kinds() { return id(&value) * 1000 + id(&ptr) * 100 + id(&items[0]) * 10 + id(&boxed.v); }
  int count() const { return N; }
};

template<class T>
struct Ref {
  T& target;
  void set(T x) { target = x; }
};

int main() {
  int x = 5;
  Mixed<int, char, 3> m{1, &x, {'a', 'b', 'c'}, {2}};
  if (m.kinds() != 1431) return 1;
  if (*m.ptr != 5) return 2;
  if (m.items[2] != 'c') return 3;
  if (m.boxed.v != 2) return 4;
  if (m.count() != 3) return 5;
  Mixed<long, long, 1> n{1L, nullptr, {2L}, {3L}};
  if (n.kinds() != 2522) return 6;
  Ref<int> r{x};
  r.set(9);
  if (x != 9) return 7;
  return 0;
}
