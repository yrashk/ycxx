// ζ f1 (#88): a forwarding reference T&&: from an lvalue T is deduced as an lvalue reference
// (int&, const int&, Box&) and the parameter collapses to an lvalue reference; from an rvalue T
// is the plain type; an explicit T = int makes it an rvalue reference, T = int& collapses
// ([temp.deduct.call], [dcl.ref]).
// EXIT: 0
int id(int*) { return 1; }
int id(const int*) { return 2; }

struct Box {
  int v;
};

// T y = x: a reference to the argument when T is a reference type, else a copy.
template<class T>
int fwd(T&& x) {
  T y = x;
  y = 42;
  return x;
}

template<class T>
int fkind(T&& x) { return id(&x); }

template<class T>
int poke(T&& b) {
  T alias = b;
  alias.v = alias.v + 1;
  return b.v;
}

int main() {
  int i = 1;
  if (fwd(i) != 42) return 1;
  if (i != 42) return 2;
  if (fwd(5) != 5) return 3;
  int j = 7;
  if (fwd(j + 0) != 7 || j != 7) return 4;
  const int ci = 3;
  if (fkind(i) != 1) return 5;
  if (fkind(ci) != 2) return 6;
  if (fkind(8) != 1) return 7;
  Box b{10};
  if (poke(b) != 11 || b.v != 11) return 8;
  if (fwd<int>(9) != 9) return 9;
  int k = 2;
  if (fwd<int&>(k) != 42 || k != 42) return 10;
  return 0;
}
