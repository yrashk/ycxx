// ι (#300): default arguments of non-static and static member functions, naming a static data
// member and an enumerator of the class declared before the function; calls through an object,
// a pointer and the class name; a default added by an out-of-class definition
// ([dcl.fct.default]/1, /6, /9, [class.mfct], [class.static.mfct]).
// EXIT: 0
struct Counter {
  enum Step { one = 1, ten = 10 };
  static int start;
  static constexpr int limit = 50;
  int value;

  Counter() : value(start) {}
  int add(int n = one) { value += n; return value; }
  int add_limit(int n = limit) const { return value + n; }
  static int make(int k = ten, int base = start) { return k + base; }
  int scaled(int a, int b);
};

int Counter::start = 3;

int Counter::scaled(int a, int b = 2) { return a * b + value; }  // adds a default for b

int main() {
  Counter c;
  if (c.value != 3) return 1;
  if (c.add() != 4) return 2;
  if (c.add(Counter::ten) != 14) return 3;
  if (c.add_limit() != 64) return 4;
  if (Counter::make() != 13) return 5;
  Counter::start = 7;
  if (Counter::make() != 17) return 6;  // the static member is read at each call
  if (c.make(1) != 8) return 7;
  Counter* p = &c;
  if (p->add() != 15) return 8;
  if (p->scaled(5) != 25) return 9;
  if (c.scaled(5, 3) != 30) return 10;
  const Counter& r = c;
  if (r.add_limit(1) != 16) return 11;
  return 0;
}
