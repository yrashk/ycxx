// θ step 1 (#279), spec-only: a function whose return type is still undeduced (only `auto f();` has been
// seen) cannot be called ([dcl.spec.auto.general]).
auto f();
int g() { return f(); } //! error
auto f() { return 1; }
int main() { return g(); }
