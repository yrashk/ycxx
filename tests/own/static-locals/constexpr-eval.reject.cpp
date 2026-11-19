// ι (#291): a constexpr static block variable must be constant-initialized ([dcl.constexpr]);
// an initializer calling a non-constexpr function is not a constant expression. A plain static
// beside it may be dynamically initialized.
int f() { return 4; }
constexpr int twice(int x) { return 2 * x; }

int g() {
  static constexpr int ok = twice(3);
  static int fine = f();
  static constexpr int bad = f(); //! error
  return ok + fine + bad;
}

int main() { return g(); }
