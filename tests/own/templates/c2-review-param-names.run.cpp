// ζ c2 (#88), from the step's review: a member defined outside its class template uses its own
// parameters' names, not the in-class declaration's ([dcl.fct]/9, [temp.mem.func]).
// EXIT: 0
int x = 100;
template<class T> struct B {
  int f(int y, int x) const;
  int g(int) const;
};
template<class T> int B<T>::f(int x, int y) const { return x - y; }
template<class T> int B<T>::g(int x) const { return x; }
int main() {
  B<int> b;
  if (b.f(5, 3) != 2) return 1;
  if (b.g(3) != 3) return 2;
  return 0;
}
