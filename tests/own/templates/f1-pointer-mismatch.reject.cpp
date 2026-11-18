// ζ f1 (#88): a parameter of type T* cannot be deduced from an int argument; deduction does not
// consider conversions ([temp.deduct.call], [temp.deduct.type]).
template<class T>
T first(T* p) { return *p; }

int main() {
  int i = 4;
  return first(i); //! error
}
