// ι (#313): two function templates both deduce to the target type and neither is more
// specialized than the other: the selection is ambiguous ([over.over], [temp.func.order]).
template <class T> int g(T, int) { return 1; }
template <class T> int g(int, T) { return 2; }

int main() {
  int (*p)(int, int) = g; //! error
  return 0;
}
