// ι (#300): a default argument added by a later declaration is not available to a call before
// that declaration ([dcl.fct.default]/4, [expr.call]/5).
int f(int a, int b);
int early() { return f(1); } //! error
int f(int a, int b = 2);
int late() { return f(1); }
