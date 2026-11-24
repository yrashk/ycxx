// #306 [class.nest]/3, [class.pre]/3: a member class declared in its class and defined later,
// in the class or at namespace scope with a qualified class-head-name.
struct Base {
  virtual ~Base() {}
  virtual int what() const { return 1; }
};

class O {
public:
  class I;
  class J;
  class J { public: int j = 2; };
  int v = 3;
};

class O::I : public Base {
public:
  int what() const override { return 7; }
  int k = 4;
};

int main() {
  O::I i;
  Base& b = i;
  O::J j;
  return b.what() + i.k + j.j + O{}.v - 16;
}
