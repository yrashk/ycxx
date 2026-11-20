// #263: deduce T from the first argument, then substitute it into the second.
// An invalid expression in the argument list removes the partial specialization
// ([temp.spec.partial.match], [temp.deduct.general]).
// EXIT: 0
template<class T, class = void> struct Pick {
  int value() { return 1; }
};

template<class T> struct Pick<T, decltype((void)T::probe())> {
  int value() { return 2; }
};

struct Valid { static int probe(); };
struct Missing {};

int main() {
  if (Pick<Valid>{}.value() != 2) return 1;
  if (Pick<Missing>{}.value() != 1) return 2;
  if (Pick<int>{}.value() != 1) return 3;
  return 0;
}
