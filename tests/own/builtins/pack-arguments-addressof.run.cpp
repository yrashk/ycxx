// #1099: __builtin_addressof takes a pack expansion as any builtin call does (Clang 23 agrees).
// XFAIL: gcc GCC 16 rejects a pack expansion as __builtin_addressof's argument.
// EXIT: 0
template <class... A> int* address(A&... a) { return __builtin_addressof(a...); }

int main() {
  int x = 0;
  return address(x) == &x ? 0 : 1;
}
