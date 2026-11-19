// θ step 2 (#280), spec-only: lambdas in member functions that read and write members and call other
// member functions through the captured `this` ([expr.prim.lambda.capture], [class.mfct.non.static]).
struct Account {
  int balance;
  int ops;
  void note() { ++ops; }
  int twice() const { return balance * 2; }
  void apply(int d) {
    auto deposit = [this](int amount) { balance += amount; note(); };
    deposit(d);
    deposit(d);
  }
  int report() const {
    auto r = [this] { return twice() + ops; };
    return r();
  }
};

int main() {
  Account a{10, 0};
  a.apply(5);
  if (a.balance != 20 || a.ops != 2) return 1;
  if (a.report() != 42) return 2;
  return 0;
}
