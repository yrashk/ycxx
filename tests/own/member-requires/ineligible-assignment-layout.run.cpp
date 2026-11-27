// [special]/6: the satisfied, more-constrained defaulted copy assignment makes the
// user-provided fallback ineligible. The spec layout reserves the base's tail padding
// (Itanium C++ ABI §1.1 and §2.4, #1417).
// XFAIL: clang@linux-x86_64 Clang reuses the base tail padding here and gives Derived size 8 (#1670)
template<class T> struct A {
  int i;
  char c;
  A& operator=(const A&) requires (sizeof(T) == 4) = default;
  A& operator=(const A&) { return *this; }
};
struct Derived : A<int> { char d; };
int main() { return sizeof(Derived) == 12 ? 0 : 1; }
