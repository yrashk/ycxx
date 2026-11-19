// θ step 1 (#279), from the review: an overloaded function's address deduces no placeholder
// ([temp.deduct.call]/6).
int f(int);
int f(long);
int main() {
  auto p = &f; //! error
  return 0;
}
