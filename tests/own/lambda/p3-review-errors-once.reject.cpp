// θ step 3 (#281), from the review: an error in a generic lambda's definition is reported once,
// not again for each specialization ([temp.res]).
int main() {
  auto f = [](auto x) { return nope + x; }; //! error
  f(1);
  f(2L);
  return 0;
}
