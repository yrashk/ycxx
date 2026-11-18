// ζ f1 (#88): f(T, int) and f(int, T) are both exact matches for f(1, 2) and neither is more
// specialized than the other: the call is ambiguous ([temp.func.order], [temp.deduct.partial],
// [over.match.best]).
template<class T>
int f(T, int) { return 1; }
template<class T>
int f(int, T) { return 2; }

int main() {
  return f(1, 2); //! error
}
