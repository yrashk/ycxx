// #807: a potentially-throwing lambda converts only to a pointer to function that may throw
// ([expr.prim.lambda.closure]/8, [conv.fctptr]).
int main() {
  auto l = []() noexcept(false) { return 0; };
  int (*p)() noexcept = l;  //! error
  return p();
}
