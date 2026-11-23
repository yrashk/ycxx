// [basic.scope.class], [class.member.lookup], [temp.names]
template<class> struct seed;
template<int> constexpr int limit() { return 0; }
struct Base {
  static constexpr int seed = 42;
  static constexpr int Derived = 8;
  template<int N> static constexpr int limit() { return N; }
};
struct Derived : Base {
  inline static constexpr bool comparison = seed < limit<50>();
  inline static constexpr auto pointer_size = sizeof(Derived*);
};
static_assert(Derived::comparison);
static_assert(Derived::pointer_size == sizeof(void*));
