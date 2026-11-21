// θ step 1 (#279), spec-only: decltype of a structured binding is the member's declared type, also for an
// array member and a reference member ([dcl.struct.bind], [dcl.type.decltype]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int g = 3;
struct R {
  int& r;
  int arr[2];
};

int main() {
  R x{g, {7, 8}};
  auto& [r, arr] = x;
  static_assert(same<decltype(r), int&>::value);
  static_assert(same<decltype(arr), int[2]>::value);
  r = 4;
  if (g != 4) return 1;
  if (arr[1] != 8) return 2;
  arr[0] = 70;
  if (x.arr[0] != 70) return 3;
  return 0;
}
