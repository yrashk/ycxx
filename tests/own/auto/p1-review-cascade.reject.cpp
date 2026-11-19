// θ step 1 (#279), from the review: after a failed deduction, uses of the variable say nothing
// more.
int f(int);
int f(long);
int main() {
  auto p = f; //! error
  return p == p ? 1 : 0;
}
