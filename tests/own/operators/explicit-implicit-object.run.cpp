// [expr.prim.lambda.closure], [over.call]: an explicit operator-function name can
// use the enclosing member's object through the lambda's capture of this.
// EXIT: 0
struct S {
  int x, y;
  int operator()(int value) { return x + value; }
  int f() {
    return [this] { return operator()(this->x + y); }();
  }
};

int main() {
  S s{10, 22};
  return s.f() != 42;
}
