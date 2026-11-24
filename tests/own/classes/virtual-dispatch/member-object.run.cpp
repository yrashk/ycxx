// #309 [class.virtual], [class.mem]: a non-polymorphic class holding polymorphic member objects;
// the members dispatch through references, and their destructors run in reverse order.
// EXIT: 0

int trace;

struct Part {
  int id;
  Part(int i) : id(i) {}
  virtual ~Part() { trace = trace * 10 + id; }
  virtual int value() { return id; }
};

struct Gear : Part {
  Gear() : Part(2) {}
  int value() override { return id * 100; }
};

struct Machine {
  int serial = 9;
  Part first{1};
  Gear second;
  int total(Part& p, Part& q) { return p.value() + q.value() + serial; }
  int run() { return total(first, second); }
};

int main() {
  {
    Machine m;
    if (m.run() != 1 + 200 + 9) return 1;
    Part& p = m.second;
    if (p.value() != 200) return 2;
    Machine copy = m;
    if (copy.run() != 210) return 3;
    trace = 0;
  }
  // copy destroyed first (2 then 1), then m (2 then 1).
  if (trace != 2121) return 4;
  return 0;
}
