// θ step 2 (#280), spec-only: `[=, this]` captures locals by copy and the object through `this`, so
// members are shared with the object while locals are copies ([expr.prim.lambda.capture]).
struct S {
  int m;
  int run() {
    int local = 1;
    auto f = [=, this] { m += local; return local; };
    local = 100;
    int r = f();
    return r * 100 + m;
  }
};

int main() {
  S s{7};
  if (s.run() != 108) return 1;
  if (s.m != 8) return 2;
  return 0;
}
