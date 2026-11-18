// δ s2 (#86): `this`: its value is the object's address, `this->m` and `(*this).m`, returning
// `this` to chain calls, a parameter that hides a member ([expr.prim.this], [class.this]).
// EXIT: 0
struct Builder {
  int value;
  int steps;

  Builder* add(int value) {
    this->value += value;  // the parameter hides the member
    (*this).steps++;
    return this;
  }
  Builder* times(int k) {
    value *= k;
    steps++;
    return this;
  }
  bool is(Builder* other) { return this == other; }
  Builder* self() { return this; }
};

int main() {
  Builder b = {0, 0};
  b.add(2)->times(5)->add(1);
  if (b.value != 11) return 1;
  if (b.steps != 3) return 2;
  if (!b.is(&b)) return 3;
  Builder c = b;
  if (c.is(&b)) return 4;
  if (b.self() != &b) return 5;
  if (c.self()->value != 11) return 6;
  return 0;
}
