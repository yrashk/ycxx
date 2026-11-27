int f() {
  const int n = 3;
  struct S { int g() { const int& r = n; return r; } }; //! error
  return S{}.g();
}
