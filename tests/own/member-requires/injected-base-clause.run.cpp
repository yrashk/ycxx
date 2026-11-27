// #1417 review [temp.dep.type], [class.member.lookup]: the injected-class-name naming a member of a
// dependent base in a member's requires-clause is looked up when the constraint is checked.
// XFAIL: ycxx the injected-class-name is reported as incomplete in a requires-clause (#1524)
template<class T> struct Base { static constexpr bool value = sizeof(T) == 4; };
template<class T> struct S : Base<T> {
  int h() requires S::value { return 1; }
  int h() requires (!S::value) { return 2; }
};
int main() { return S<int>{}.h() == 1 && S<char>{}.h() == 2 ? 0 : 1; }
