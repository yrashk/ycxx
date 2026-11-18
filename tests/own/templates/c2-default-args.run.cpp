// ζ c2 (#88): default template arguments of a class template: Box<> takes every default, a
// default may name an earlier parameter, an explicit argument replaces a default, and a
// specialization named with its defaults spelt out is the same type ([temp.param], [temp.arg],
// [temp.names], [temp.type]).
// EXIT: 0
int id(int*) { return 1; }
int id(long*) { return 2; }
int id(char*) { return 3; }

template<class T = long, int N = 2>
struct Store {
  T items[N];
  int count() const { return N; }
  int kind() { return id(&items[0]); }
};

template<class T, class U = T>
struct Pair {
  T first;
  U second;
  int kinds() { return id(&first) * 10 + id(&second); }
};

int which(Store<long, 2>&) { return 1; }
int which(Store<int, 2>&) { return 2; }
int which(Store<int, 5>&) { return 3; }

int main() {
  Store<> a{};
  if (a.count() != 2) return 1;
  if (a.kind() != 2) return 2;
  Store<int> b{};
  if (b.kind() != 1) return 3;
  if (b.count() != 2) return 4;
  Store<char, 6> c{};
  if (c.kind() != 3) return 5;
  if (c.count() != 6) return 6;
  if (which(a) != 1) return 7;
  if (which(b) != 2) return 8;
  Store<int, 5> d{};
  if (which(d) != 3) return 9;
  Pair<int> p{1, 2};
  if (p.kinds() != 11) return 10;
  Pair<char, long> q{'a', 3L};
  if (q.kinds() != 32) return 11;
  return 0;
}
