int f() {
  const int n = 3;
  return [] { const int& r = n; return r; }(); //! error
}
