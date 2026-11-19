// ι (#300): the out-of-class definition of a member function of a non-template class may add a
// default argument, which calls after it use ([dcl.fct.default]/6).
// XFAIL: ycxx only the in-class declaration's default arguments are recorded (#354)
// EXIT: 0
struct Counter {
  int value = 5;
  int scaled(int a, int b);
};

int Counter::scaled(int a, int b = 2) { return a * b + value; }  // adds a default for b

int main() {
  Counter c;
  Counter* p = &c;
  if (p->scaled(5) != 15) return 1;
  if (c.scaled(5, 3) != 20) return 2;
  return 0;
}
