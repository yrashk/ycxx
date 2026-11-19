// ι (#298), from the review: in a template's definition too, the loop variable is not
// redeclared in the outermost block of the body ([basic.scope.block]/2).
template <class R> int f(R& r) {
  for (auto x : r) {
    int x = 0; //! error
  }
  return 0;
}
int main() { return 0; }
