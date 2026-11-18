// ζ f1 (#88): default template arguments of function templates: used when a parameter is
// neither specified nor deduced, replaced by an explicit argument, not used when deduction
// succeeds, naming an earlier parameter, and followed by a parameter without a default, which
// function templates allow when it is deduced ([temp.param], [temp.deduct.general],
// [temp.arg.explicit]).
// EXIT: 0
int id(int*) { return 1; }
int id(long*) { return 2; }
int id(char*) { return 3; }

template<class T = long>
int which() {
  T x = 0;
  return id(&x);
}

template<class T = long, int N = 3>
T make() { return N; }

template<class T, class U = T>
U convert(T x) { return x; }

template<class R = long, class T>
int widen_kind(T a) {
  R r = a;
  return id(&r);
}

template<class T = char>
int deduced_wins(T x) { return id(&x); }

int main() {
  if (which() != 2) return 1;
  if (which<int>() != 1) return 2;
  if (which<char>() != 3) return 3;
  if (make() != 3) return 4;
  if (make<int>() != 3) return 5;
  if (make<int, 7>() != 7) return 6;
  if (convert(300) != 300) return 7;
  if (convert<int, char>(65) != 'A') return 8;
  if (convert<int, bool>(5) != true) return 9;
  if (widen_kind(1) != 2) return 10;
  if (widen_kind<char>(1) != 3) return 11;
  if (widen_kind<int>('a') != 1) return 12;
  if (deduced_wins(1000) != 1) return 13;
  if (deduced_wins(7L) != 2) return 14;
  return 0;
}
