// Contracts (P2900): preconditions, postconditions naming the result, and contract_assert, on
// paths that hold; contract_violation.cpp checks that a violation ends the program.
import std;
import bootstrap.smoke;

namespace {

int checked_div(const int a, const int b)
  pre(b != 0)
  post(r : r * b <= a)
{
  return a / b;
}

class counter {
public:
  void add(int n)
    pre(n >= 0)
  {
    value_ += n;
    contract_assert(value_ >= 0);
  }
  int value() const
    post(r : r >= 0)
  {
    return value_;
  }

private:
  int value_ = 0;
};

constexpr int twice(int x)
  pre(x < 1000)
{
  return 2 * x;
}

}  // namespace

int main() {
  smoke::check(checked_div(7, 2) == 3, "pre and post hold");
  counter c;
  c.add(2);
  c.add(3);
  smoke::check(c.value() == 5, "member contracts");
  static_assert(twice(21) == 42, "contracts in constant evaluation");
  return smoke::result("contracts");
}
