// #309 [class.abstract], [class.virtual]: pure virtual functions in an abstract base, overridden
// in concrete derived classes; the abstract class is used only as a base, pointer and reference.
// EXIT: 0

struct Counter {
  virtual int next() = 0;
  virtual int step() = 0;
  virtual ~Counter() {}
  int twice() { return next() + next(); }
};

struct Linear : Counter {
  int value = 0;
  int step() override { return 1; }
  int next() override { value += step(); return value; }
};

struct ByThree : Linear {
  int step() override { return 3; }
};

// An abstract class derived from an abstract class keeps an inherited pure virtual.
struct Half : Counter {
  int next() override { return 40 + step(); }
};

struct Whole : Half {
  int step() override { return 2; }
};

int run(Counter& c) { return c.twice(); }

int main() {
  Linear l;
  ByThree t;
  Whole w;
  if (run(l) != 3) return 1;
  if (run(t) != 9) return 2;
  if (run(w) != 84) return 3;
  Counter* p = &t;
  if (p->next() != 9) return 4;
  if (p->step() != 3) return 5;
  return 0;
}
