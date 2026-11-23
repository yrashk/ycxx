// [expr.type.conv], [expr.ref], [dcl.ambig.res]
struct Product {
  int value;
  Product(int input) : value(input) {}
};
struct Traits { using type = Product; };

template<class T> int read(int input) {
  int result(typename T::type(((input))).value);
  return result;
}
int main() { return read<Traits>(21) != 21; }
