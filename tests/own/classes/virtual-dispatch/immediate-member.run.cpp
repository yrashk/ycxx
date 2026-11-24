// #1097 [class.virtual]: immediate members coexist with ordinary virtual dispatch.
// EXIT: 0

struct Base {
  virtual consteval int compile_time_value() const { return 2; }
  virtual int value() const { return 17; }
};

struct Derived : Base {
  consteval int compile_time_value() const override { return 3; }
  int value() const override { return 29; }
};

int read_value(const Base& object) { return object.value(); }

int main() {
  Base b;
  Derived d;
  return read_value(b) == 17 && read_value(d) == 29 ? 0 : 1;
}
