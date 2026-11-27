// #340: a private static data member template is accessible inside its class but not from
// outside it ([class.access], [temp.mem]).
struct C {
private:
  template <class T> static constexpr int hidden = 1;
public:
  static int inside() { return hidden<int>; }
};
template <class A> struct D {
private:
  template <class T> static constexpr int hidden = 2;
};
int a = C::hidden<int>;  //! error
int b = D<int>::hidden<char>;  //! error
int main() { return C::inside() - 1; }
