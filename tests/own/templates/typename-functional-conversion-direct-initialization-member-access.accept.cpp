// [dcl.init], [dcl.ambig.res], [expr.ref]
struct Result {
  int value;
  Result(int seed) : value(seed) {}
};
struct Traits { using type = Result; };
template<class T> int check(int seed) {
  int result(typename T::type(seed).value);
  return result;
}
int main() { return check<Traits>(17) != 17; }
