// #157 [class.dtor], [dcl.fct.def.default]: first-declaration defaulting permits trivial
// destruction and preserves the access of the declared destructor.
// EXIT: 0
struct Reading {
  int value;
  ~Reading() = default;
};

class Exclusive {
  int value_;
public:
  Exclusive(int value) : value_(value) {}
  Exclusive(const Exclusive&) = delete;
  Exclusive& operator=(const Exclusive&) = delete;
  ~Exclusive() = default;
  int value() const { return value_; }
};

class Hidden {
  int value_;
  ~Hidden() = default;

  friend int read_hidden(int value);

public:
  Hidden(int value) : value_(value) {}
  int value() const { return value_; }
};

int read_pointer(const Hidden* object) {
  return object->value();
}

int read_hidden(int value) {
  const Hidden object(value);
  return read_pointer(&object);
}

int main() {
  const Reading reading{8};
  {
    const Exclusive exclusive(13);
    if (exclusive.value() != 13) return 1;
  }
  if (read_hidden(21) != 21) return 2;
  return reading.value != 8;
}
