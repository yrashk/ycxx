// ι (#291): a static block variable in a member function, or in a static member function, is
// one variable shared by every object of the class and every call ([stmt.dcl]/3,
// [class.mfct]); a non-static member function's static is not per object.
// EXIT: 0
int inits = 0;

int mark(int v) {
  ++inits;
  return v;
}

struct Counter {
  int own;
  int tick() {
    static int shared = mark(0);
    ++own;
    return ++shared;
  }
  static int next_id() {
    static int id = mark(100);
    return id++;
  }
  int peek() const;
};

// Defined outside the class: not inline, the same rule.
int Counter::peek() const {
  static int reads = 0;
  return ++reads + own * 0;
}

struct Other {
  int tick() {
    static int shared = 50;
    return ++shared;
  }
};

int main() {
  Counter a{0};
  Counter b{0};
  if (a.tick() != 1) return 1;
  if (b.tick() != 2) return 2;
  if (a.tick() != 3) return 3;
  if (a.own != 2 || b.own != 1) return 4;
  if (inits != 1) return 5;

  if (Counter::next_id() != 100) return 6;
  if (a.next_id() != 101) return 7;
  if (Counter::next_id() != 102) return 8;
  if (inits != 2) return 9;

  const Counter c{0};
  if (c.peek() != 1) return 10;
  if (a.peek() != 2) return 11;

  Other o;
  if (o.tick() != 51) return 12;
  if (a.tick() != 4) return 13;
  return 0;
}
