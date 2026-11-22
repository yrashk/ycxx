// #297: pack-index-specifiers `T...[I]` ([dcl.type.pack.index]) in alias templates, function
// signatures (leading and trailing return types, parameters, a variable template), bodies (with an
// index computed from `sizeof...`) and a type-trait operand. The index is a constant or a non-type
// parameter. Also an alias applied to another template's pack, and an explicitly specified pack,
// whose element type is a non-deduced context ([temp.deduct.type]/5) the argument converts to.
// EXIT: 0
template<class A, class B> constexpr bool same = false;
template<class A> constexpr bool same<A, A> = true;

template<class... T> using First = T...[0];
template<int I, class... T> using Nth = T...[I];
static_assert(same<First<int, char>, int>);
static_assert(same<Nth<1, int, char, long>, char>);
static_assert(same<Nth<2, int, char, const long&>, const long&>);
static_assert(same<const First<int>, const int>);

template<class... T> struct Box { using front = First<T...>; using second = T...[1]; };
static_assert(same<Box<short, char>::front, short>);
static_assert(same<Box<short, char>::second, char>);

template<class... T> int body() {
  using A0 = T...[0];
  using R = __remove_cvref(T...[0]);
  using Last = T...[sizeof...(T) - 1];
  Last unused = 0;
  (void)unused;
  A0 a = 3;
  R r = a;
  return sizeof(T...[1]) + r;
}
template<class... T> T...[0] first(T... t) { return 5; }
template<class... T> auto last(T... t) -> T...[1] { return 6; }
template<int I, class... T> T...[I] at(T... t) { return 7; }
template<class... T> T...[1] second_v = 8;
template<class... T> int sum(T...[0] x, T...[0] y, T...[0]* p) { return x + y + (p ? 1 : 0); }

int main() {
  if (body<const int&, long>() != int(sizeof(long)) + 3) return 1;
  if (first(1, 2.0) != 5 || last(1.0, 2) != 6) return 2;
  if (at<1>(1.5, 2) != 7 || second_v<char, int> != 8) return 3;
  if (sum<int>(1, 2, nullptr) != 3) return 4;
  return 0;
}
