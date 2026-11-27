// #340: naming a static data member template with arguments that do not satisfy its constraints
// is ill-formed ([temp.names]/8, [temp.constr.decl], [temp.mem]).
template <class T> concept Small = sizeof(T) <= 2;
struct C {
  template <class T> requires (sizeof(T) == 1) static constexpr bool one = true;
  template <Small T> static constexpr int size = int(sizeof(T));
};
template <class A> struct D {
  template <class T> requires (sizeof(T) <= sizeof(A)) static constexpr bool c = true;
};
bool a = C::one<int>;  //! error
int b = C::size<long long>;  //! error
bool c = D<char>::c<int>;  //! error
int main() { return 0; }
