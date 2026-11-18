// ζ f1 (#88): a function template specialization with a better conversion sequence beats a
// non-template: an exact match over a promotion, over a conversion, over a qualification
// conversion, and a direct reference binding over a temporary ([temp.over], [over.match.best],
// [over.ics.rank]).
// EXIT: 0
int h(int) { return 1; }
template<class T>
int h(T) { return 2; }

int k(long) { return 1; }
template<class T>
int k(T) { return 2; }

int m(const int&) { return 1; }
template<class T>
int m(const T&) { return 2; }

int q(const int*) { return 1; }
template<class T>
int q(T*) { return 2; }

int main() {
  if (h('c') != 2) return 1;
  if (h(true) != 2) return 2;
  if (h(5L) != 2) return 3;
  short s = 4;
  if (h(s) != 2) return 4;
  if (k(5) != 2) return 5;
  if (k(5L) != 1) return 6;
  if (m('a') != 2) return 7;
  if (m(1) != 1) return 8;
  int i = 0;
  const int ci = 0;
  if (q(&i) != 2) return 9;
  if (q(&ci) != 1) return 10;
  return 0;
}
