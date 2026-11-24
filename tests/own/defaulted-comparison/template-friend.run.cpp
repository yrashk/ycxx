// [class.compare.default]: a class template's defaulted friend `operator==`, and the rewritten
// `!=` that uses it ([over.match.oper]).
template <class T> struct W {
  T v;
  friend constexpr bool operator==(W, W) noexcept = default;
};
int main() { return (W<int>{1} == W<int>{1} && W<int>{1} != W<int>{2}) ? 0 : 1; }
