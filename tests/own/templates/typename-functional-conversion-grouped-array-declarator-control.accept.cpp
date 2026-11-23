// [stmt.ambig], [dcl.array]
struct Traits { using type = int; };
template<class T> int check() {
  typename T::type(values)[2] = {3, 7};
  return values[0] + values[1];
}
int main() { return check<Traits>() != 10; }
