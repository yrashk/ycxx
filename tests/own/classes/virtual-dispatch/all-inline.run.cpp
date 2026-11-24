// #309 [class.virtual]: classes whose virtual functions are all defined inline in the class,
// declared in a named namespace and derived from across namespaces.
// EXIT: 0

namespace outer {
struct Animal {
  virtual int legs() { return 0; }
  virtual int noise() { return legs() + 1; }
};

struct Dog : Animal {
  int legs() override { return 4; }
};
}  // namespace outer

struct Bird : outer::Animal {
  int legs() override { return 2; }
  int noise() override { return 20 + outer::Animal::noise(); }
};

int count(outer::Animal& a) { return a.noise(); }

int main() {
  outer::Animal a;
  outer::Dog d;
  Bird b;
  if (count(a) != 1) return 1;
  if (count(d) != 5) return 2;
  if (count(b) != 23) return 3;
  outer::Animal* p = &d;
  if (p->legs() != 4) return 4;
  return 0;
}
