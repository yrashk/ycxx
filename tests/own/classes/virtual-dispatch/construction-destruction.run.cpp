// #309 [class.cdtor]: a virtual call during a base constructor or destructor body, directly or
// through a member function, calls the version of the class under construction or destruction.
// EXIT: 0

int trace;

struct Base {
  Base() { trace = trace * 10 + who(); helper(); }
  virtual ~Base() { trace = trace * 10 + who(); helper(); }
  virtual int who() { return 1; }
  void helper() { trace = trace * 10 + who(); }
};

struct Middle : Base {
  Middle() { trace = trace * 10 + who(); }
  ~Middle() override { trace = trace * 10 + who(); }
  int who() override { return 2; }
};

struct Leaf : Middle {
  int who() override { return 3; }
  void probe() { trace = trace * 10 + who(); }
};

int main() {
  {
    Leaf l;
    if (trace != 112) return 1;
    trace = 0;
    l.probe();
    Base& b = l;
    if (b.who() != 3) return 2;
    if (trace != 3) return 3;
    trace = 0;
  }
  if (trace != 211) return 4;
  return 0;
}
