// [stmt.ambig], [expr.type.conv]: the pointer operand and trailing operator can require an expression.
namespace api {
using Argument = int;
using Callback = int (*)(Argument);
}

int plus_one(int value) { return value + 1; }
int plus_two(int value) { return value + 2; }

bool probe() {
  api::Callback callbacks[] = {&plus_one, &plus_two};
  api::Callback *pp = callbacks;
  int argument = 3;
  int score = 0;
  api::Callback (*pp)(api::Argument(argument)) + 4 == 8
      ? ++score : --score;
  api::Callback (*++pp)(api::Argument(argument));
  api::Callback (*pp++)(api::Argument(argument));
  return score == 1 && pp == callbacks + 2;
}
