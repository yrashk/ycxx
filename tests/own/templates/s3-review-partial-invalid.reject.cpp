// ζ s3 (#88), from the step's review: a partial specialization no more specialized than its
// primary template, and ones with a parameter that cannot be deduced
// ([temp.spec.partial.general]).
template<class T, class U> struct P { };
template<class T, class U> struct P<U, T> { }; //! error
template<class T, class U> constexpr int v = 0;
template<class T, class U> constexpr int v<T*, int> = 1; //! error
template<class T> struct X { };
template<class U> struct X<int> { }; //! error
int main() { return 0; }
