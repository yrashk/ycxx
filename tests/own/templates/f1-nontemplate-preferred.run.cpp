// ζ f1 (#88): a non-template function and a function template specialization with equally good
// conversion sequences: the non-template is better, whether it is declared before or after the
// template, and for value and reference parameters ([temp.over], [over.match.best]).
// EXIT: 0
int h(int) { return 1; }
template<class T>
int h(T) { return 2; }

template<class T>
int r(const T&) { return 2; }
int r(const int&) { return 1; }

template<class T>
int p(T*) { return 2; }
int p(char*) { return 1; }

struct S {
  int v;
};
template<class T>
int q(T&) { return 2; }
int q(S&) { return 1; }

int main() {
  int i = 0;
  if (h(5) != 1) return 1;
  if (h(i) != 1) return 2;
  const int ci = 1;
  if (h(ci) != 1) return 3;
  if (r(3) != 1) return 4;
  if (r(i) != 1) return 5;
  char c = 'a';
  if (p(&c) != 1) return 6;
  if (p(&i) != 2) return 7;
  S s{1};
  if (q(s) != 1) return 8;
  if (q(i) != 2) return 9;
  return 0;
}
