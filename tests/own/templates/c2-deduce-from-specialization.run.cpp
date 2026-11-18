// ζ c2 (#88): function templates deduce their parameters from a class template specialization
// in the argument's type: T from const Box<T>&, from Box<T>*, from a nested Box<Box<T>>&, two
// parameters from Pair<T, U>&, and partial ordering prefers the more specialized
// Box<Box<T>>& over Box<T>& ([temp.deduct.type], [temp.deduct.call], [temp.func.order]).
// EXIT: 0
int id(int*) { return 1; }
int id(long*) { return 2; }
int id(char*) { return 3; }

template<class T>
struct Box {
  T v;
};

template<class T, class U>
struct Pair {
  T first;
  U second;
};

template<class T>
T get(const Box<T>& b) { return b.v; }

template<class T>
T* addr(Box<T>* p) { return &p->v; }

template<class T>
int kind(Box<T>&) { T t = 0; return id(&t); }

template<class T>
T inner(Box<Box<T>>& b) { return b.v.v; }

template<class T, class U>
int pair_kinds(Pair<T, U>& p) { return id(&p.first) * 10 + id(&p.second); }

template<class T>
int depth(Box<T>&) { return 1; }

template<class T>
int depth(Box<Box<T>>&) { return 2; }

int main() {
  Box<int> a{3};
  if (get(a) != 3) return 1;
  Box<long> l{4L};
  if (get(l) != 4L) return 2;
  if (addr(&a) != &a.v) return 3;
  *addr(&l) = 9L;
  if (l.v != 9L) return 4;
  Box<char> c{'c'};
  if (kind(a) != 1) return 5;
  if (kind(l) != 2) return 6;
  if (kind(c) != 3) return 7;
  Box<Box<int>> bb{{8}};
  if (inner(bb) != 8) return 8;
  Pair<long, char> p{1L, 'x'};
  if (pair_kinds(p) != 23) return 9;
  if (depth(a) != 1) return 10;
  if (depth(bb) != 2) return 11;
  return 0;
}
