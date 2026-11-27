// #1417 review [temp.expl.spec]: an explicit specialization of a constrained member of a class
// template specialization names the member whose constraints that specialization satisfies.
template<class T> struct One {
  int f() requires (sizeof(T) == 4) { return 1; }
};
template<class T> struct Two {
  int f() requires (sizeof(T) == 4) { return 1; }
  int f() requires (sizeof(T) != 4) { return 2; }
  int g() { return 5; }
};
template<> int One<int>::f() { return 10; }
template<> int Two<int>::f() { return 20; }
template<> int Two<int>::g() { return 50; }
int main() {
  return One<int>{}.f() == 10 && Two<int>{}.f() == 20 && Two<char>{}.f() == 2 && Two<int>{}.g() == 50 ? 0 : 1;
}
