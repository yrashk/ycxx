// #309 [class.dtor], [class.virtual]: destroying a derived automatic object runs the derived
// destructor body, then the base's; an implicit destructor overrides a virtual base destructor;
// `virtual ~B() = default;`, `virtual ~B() {}`, and an out-of-class `B::~B() {}`.
// EXIT: 0

int trace;

struct Defaulted {
  virtual ~Defaulted() = default;
  virtual int id() { return 1; }
};

struct Braced {
  virtual ~Braced() {}
  virtual int id() { return 2; }
};

struct OutOfClass {
  virtual ~OutOfClass();
  virtual int id() { return 3; }
};

OutOfClass::~OutOfClass() { trace = trace * 10 + 1; }

struct Middle : OutOfClass {
  ~Middle() override { trace = trace * 10 + 2; }
  int id() override { return 4; }
};

// The implicit destructor of Leaf is virtual: it overrides Middle's.
struct Leaf : Middle {
  int id() override { return 5; }
};

struct Last : Leaf {
  ~Last() { trace = trace * 10 + 3; }
};

struct FromDefaulted : Defaulted {
  ~FromDefaulted() { trace = trace * 10 + 7; }
};

struct FromBraced : Braced {
  ~FromBraced() { trace = trace * 10 + 8; }
};

int main() {
  { Middle m; }
  if (trace != 21) return 1;
  trace = 0;
  { Leaf l; if (l.id() != 5) return 2; }
  if (trace != 21) return 3;
  trace = 0;
  {
    Last x;
    OutOfClass& r = x;
    if (r.id() != 5) return 4;
  }
  if (trace != 321) return 5;
  trace = 0;
  { FromDefaulted d; Defaulted& r = d; if (r.id() != 1) return 6; }
  if (trace != 7) return 7;
  trace = 0;
  { FromBraced b; Braced& r = b; if (r.id() != 2) return 8; }
  if (trace != 8) return 9;
  trace = 0;
  { OutOfClass o; }
  if (trace != 1) return 10;
  return 0;
}
