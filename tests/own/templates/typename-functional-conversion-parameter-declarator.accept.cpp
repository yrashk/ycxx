// [dcl.ambig.res], [dcl.fct], [temp.res]
struct Traits { using type = int; };
int target(int value) { return value + 1; }
template<class T> int check() {
  int (*declared)(typename T::type(value)) = target;
  return declared(31);
}
int main() { return check<Traits>() != 32; }
