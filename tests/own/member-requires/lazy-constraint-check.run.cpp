// #1417 [temp.inst], [temp.constr.atomic]: the requires-clause of a member function is not
// instantiated with the class; it is checked only when the member is a candidate. A member
// whose constraint check would be a hard error for this T (instantiating `Explodes<T>` fails
// a static_assert) does not break instantiating the class or calling other members.
template<class T>
struct Explodes {
  static_assert(sizeof(T) == 0, "instantiated");
  static constexpr bool value = true;
};

template<class T>
struct Lazy {
  T v;
  int trap() requires Explodes<T>::value { return 1; }
  int safe() const { return int(v) + 1; }
  static int also_safe() { return 7; }
};

int main() {
  Lazy<int> l{4};
  if (l.safe() != 5) return 1;
  if (Lazy<int>::also_safe() != 7) return 2;
  return sizeof(Lazy<char>) == sizeof(char) ? 0 : 3;
}
