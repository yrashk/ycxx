// The inherited non-template hides the namespace alias ([basic.lookup.unqual]).
template<class> using Pick = int;

struct Base { using Pick = long long; };
template<class T> struct Derived : Base {
  static int result() {
    using Type = Pick<int>;  //! error
    return 0;
  }
};

int result = Derived<int>::result();
