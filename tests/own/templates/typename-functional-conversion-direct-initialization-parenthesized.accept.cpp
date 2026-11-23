// [dcl.init], [expr.type.conv], [dcl.ambig.res]
struct Traits { using type = int; };
template<class T> int check(int seed) {
  int result(typename T::type(seed + 1));
  return result;
}
int main() { return check<Traits>(17) != 18; }
