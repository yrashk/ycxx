// [dcl.init], [dcl.ambig.res], [expr.mul]
struct Traits { using type = int; };
template<class T> int check(int seed) {
  int result(typename T::type(seed * 2));
  return result;
}
int main() { return check<Traits>(17) != 34; }
