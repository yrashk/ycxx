// An ambiguous member lookup cannot fall back to the namespace alias ([class.member.lookup]).
template<class> using Pick = int;

struct Left { template<class> using Pick = char; };
struct Right { template<class> using Pick = long long; };
struct Base : Left, Right {};
template<class T> struct Derived : Base {
  static int result() {
    using Type = Pick<int>;  //! error
    return 0;
  }
};

int result = Derived<int>::result();
