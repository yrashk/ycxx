// #133, from the step's review: a statement beginning with a functional cast is an expression
// unless it can be a declaration ([stmt.ambig]); a std::nullptr_t object converts to a pointer
// ([conv.ptr]/1).
// EXIT: 0
int take(int* p) { return p == nullptr; }
int f(int x) {
  int(x) + 1;
  int(3);
  void();
  int(y) = 4;
  int(z);
  z = 1;
  return x + y + z + int(x);
}
int main() {
  decltype(nullptr) a = nullptr;
  int* p = a;
  return f(1) - 7 + take(a) + (p == nullptr) - 2;
}
