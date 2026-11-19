// θ step 3 (#281), spec-only: a generic lambda in a member function capturing `this` (explicitly, by `[=,
// this]` and by `[&]`) or `*this` reaches the object's members from each specialization
// ([expr.prim.lambda.capture], [expr.prim.lambda.closure], [expr.prim.this]).
struct Counter {
  int n = 0;
  int bumpAll() {
    auto add = [this](auto x) { n += x; return n; };
    add(1);
    add('\2');
    add(3L);
    auto twice = [=, this](auto x) { return n * 2 + x; };
    auto viaRef = [&](auto x) { n = n + static_cast<int>(x); };
    viaRef('\4');
    return twice(0L) + twice(1);
  }
  int snapshot() {
    auto snap = [*this](auto x) { return n + x; };
    n = 1000;
    return snap(0) + snap('\0');
  }
};

int main() {
  Counter c;
  if (c.bumpAll() != 41) return 1;
  if (c.n != 10) return 2;
  if (c.snapshot() != 20) return 3;
  if (c.n != 1000) return 4;
  return 0;
}
