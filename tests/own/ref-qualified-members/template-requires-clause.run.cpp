// [dcl.decl.general], [dcl.fct], [temp.pre]: after a ref-qualifier or a trailing return type ending in
// `&&`, `requires` begins the trailing requires-clause; the member's body is its own.
// EXIT: 173
template <class T> struct W {
  int a() && requires (sizeof(T) > 0) { return 1; }
  int b() { return 2; }
  int c() & requires (sizeof(T) > 0) && requires { sizeof(T); } { return 4; }
  int c() && requires (sizeof(T) > 0) && requires { sizeof(T); } { return 8; }
  auto d() -> T&& requires (sizeof(T) > 0) { return static_cast<T&&>(t); }
  int e() { return 16; }
  template <class U> int f() const && requires (sizeof(U) > 0) { return 32; }
  int g() { return 64; }
  T t{};
};
struct S { template <class U> int a() && requires (sizeof(U) > 0) { return 1; } int b() { return 2; } };
template <class T> struct Last { int a() && requires (sizeof(T) > 0) { return 128; } };
int main() {
  W<int> w;
  return W<int>{}.a() + w.c() + W<int>{}.c() + (w.d() == 0 ? 0 : 99) + static_cast<const W<int>&&>(w).f<int>()
       + S{}.a<int>() - 1 + Last<int>{}.a();
}
