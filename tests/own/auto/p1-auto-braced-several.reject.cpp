// θ step 1 (#279), spec-only: direct-list-initialization of an `auto` variable needs a braced list of
// exactly one element ([dcl.type.auto.deduct]).
int main() {
  auto x{1, 2}; //! error
  return 0;
}
