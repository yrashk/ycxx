// θ step 1 (#279), spec-only: a recursive call before any return statement has deduced the type uses a
// function with an undeduced return type ([dcl.spec.auto.general]).
auto f() { return f(); } //! error
int main() { return 0; }
