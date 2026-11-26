// #1417 [temp.constr.atomic], [temp.inst], [over.match.viable]: when substituting T into a
// member's requires-clause forms an invalid type or expression, the constraint is not
// satisfied (not ill-formed); the member is simply not viable and another overload is used.
struct WithValue { static constexpr int value = 3; using type = long; };
struct Without {};

template<class T>
struct Probe {
  static int f() requires (T::value > 0) { return T::value; }
  static int f() requires (!requires { T::value; }) { return -1; }

  static int g() requires (sizeof(typename T::type) > 1) { return 1; }
  static int g() { return 0; }
};

int main() {
  if (Probe<WithValue>::f() != 3) return 1;
  if (Probe<Without>::f() != -1) return 2;
  if (Probe<int>::f() != -1) return 3;
  if (Probe<WithValue>::g() != 1) return 4;
  if (Probe<Without>::g() != 0) return 5;
  return 0;
}
