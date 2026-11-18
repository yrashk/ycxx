// ζ s3 (#88): partial specializations of a class template (for pointers, pointers to const, and
// arrays with the bound deduced) and of a variable template (for references, and for two equal
// arguments): the most specialized one that matches is used, and the primary template otherwise
// ([temp.spec.partial], [temp.spec.partial.match], [temp.spec.partial.order]).
// EXIT: 0
template<class T>
struct Kind {
  int get() const { return 0; }
};
template<class T>
struct Kind<T*> {
  int get() const { return 1; }
  T* null() const { return nullptr; }
};
template<class T>
struct Kind<const T*> {
  int get() const { return 2; }
};
template<class T, int N>
struct Kind<T[N]> {
  int get() const { return N; }
};

template<class T> constexpr bool is_lref = false;
template<class T> constexpr bool is_lref<T&> = true;
template<class T, class U> constexpr int same = 0;
template<class T> constexpr int same<T, T> = 1;
static_assert(is_lref<int&> && !is_lref<int>);
static_assert(same<int, int> == 1 && same<int, long> == 0);

int main() {
  Kind<int> a;
  Kind<int*> b;
  Kind<const char*> c;
  Kind<int[7]> d;
  if (a.get() != 0 || b.get() != 1 || c.get() != 2 || d.get() != 7) return 1;
  if (b.null() != nullptr) return 2;
  return 0;
}
