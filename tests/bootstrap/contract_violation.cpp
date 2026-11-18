// A violated precondition calls the replaceable violation handler ([basic.contract.handler]).
// The handler reports and exits; CTest checks its message.
import std;

void handle_contract_violation(const std::contracts::contract_violation& v) {
  std::println("violated: {}", v.comment());
  std::exit(0);
}

int positive(int x)
  pre(x > 0)
{
  return x;
}

int main(int argc, char**) {
  std::println("{}", positive(1 - argc));  // argc is 1: the argument is 0
  return 1;
}
