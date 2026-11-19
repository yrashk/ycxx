// #337 (ι, review): a goto, case or default label outside a substatement of `if constexpr` or
// `if consteval` may not jump into it ([stmt.if]/2, /4).
int f(int x) { goto L; if constexpr (true) { L: return 1; } return 0; } //! error
int g(int x) { switch (x) { if constexpr (true) { case 1: return 1; } } return 0; } //! error
int h(int x) { goto L; if consteval { return 2; } else { L: return 1; } return 0; } //! error
int m(int x) { if (x) goto L; if constexpr (false) { L: return 1; } return 0; } //! error
int ok(int x) { if constexpr (true) { goto out; } out: return x; }  // jumping out is fine
int main() { return f(0) + g(0) + h(0) + m(0) + ok(0); }
