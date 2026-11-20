// #354: a member default sees later class members, while an earlier parameter
// hides a member of the same name ([class.mem.general], [basic.scope.param], [dcl.fct.default]).
// EXIT: 0
struct Source {
  int low;
  template<int A, int B> int get() const { return A + B; }
};

struct S {
  int compare(int below = low < high, int above = high > low);
  unsigned size(char value, unsigned n = sizeof(value)) { return n; }
  int invoke(int sum = source.get<1, 2>(), int below = source.low < high) { return sum + below; }

  static int low;
  static int high;
  static int value;
  static Source source;
};

int S::low = 1;
int S::high = 2;
Source S::source{1};
int S::compare(int below, int above) { return below + above; }

int main() {
  S s;
  if (s.compare() != 2) return 1;
  if (s.compare(0) != 1) return 2;
  if (s.size('x') != sizeof(char)) return 3;
  if (s.invoke() != 4) return 4;
  return 0;
}
