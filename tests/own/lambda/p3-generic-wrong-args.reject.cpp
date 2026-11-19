// θ step 3 (#281), spec-only: a generic lambda with two `auto` parameters called with one argument has
// no viable function call operator: the template has exactly two function parameters
// ([expr.prim.lambda.closure], [temp.deduct.call], [over.call.object], [over.match.viable]).
int main() {
  auto add = [](auto a, auto b) { return a + b; };
  return add(1); //! error
}
