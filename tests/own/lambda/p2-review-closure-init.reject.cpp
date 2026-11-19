// θ step 2 (#280), from the review: a closure type is not an aggregate: no list-initialization
// from its captures ([expr.prim.lambda.closure]/1, [dcl.init.list]).
int main() {
  int x = 1;
  auto l = [x] { return x; };
  decltype(l) m{5}; //! error
  return m();
}
