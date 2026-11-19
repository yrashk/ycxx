// θ step 2 (#280), spec-only: `[this]` captures the object by reference: members named in the lambda
// are accessed through the captured `this`, read and written ([expr.prim.lambda.capture],
// [expr.prim.this], [class.mfct.non.static]).
struct Counter {
  int value;
  int step;
  void bump() { [this] { value += step; }(); }
  int peek() {
    auto get = [this] { return this->value * 10 + step; };
    value = 4;
    return get();
  }
};

int main() {
  Counter c{1, 2};
  c.bump();
  if (c.value != 3) return 1;
  if (c.peek() != 42) return 2;
  return 0;
}
