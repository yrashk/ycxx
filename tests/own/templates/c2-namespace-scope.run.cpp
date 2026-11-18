// ζ c2 (#88): class templates declared in a named namespace and in a nested one, named with
// qualified template-ids (geo::Point<int>, geo::detail::Tag<3>) and after a using-declaration;
// a member defined outside the class within its namespace, and one defined at global scope with
// a qualified name ([temp.class], [temp.names], [namespace.def], [namespace.udecl],
// [temp.mem.func]).
// EXIT: 0
namespace geo {
  template<class T>
  struct Point {
    T x;
    T y;
    T sum() const;
    T diff() const;
  };

  template<class T>
  T Point<T>::sum() const { return x + y; }

  namespace detail {
    template<int N>
    struct Tag {
      int get() const { return N; }
    };
  }
}

template<class T>
T geo::Point<T>::diff() const { return x - y; }

int norm1(const geo::Point<int>& p) { return p.sum(); }

int main() {
  geo::Point<int> p{3, 4};
  if (p.sum() != 7) return 1;
  if (p.diff() != -1) return 2;
  if (norm1(p) != 7) return 3;
  geo::detail::Tag<3> t{};
  if (t.get() != 3) return 4;
  using geo::Point;
  Point<long> q{10L, 4L};
  if (q.diff() != 6L) return 5;
  return 0;
}
