// The other side of abi-callbacks.run.cpp, compiled by Clang.
typedef double (*BinOp)(double, double);
typedef float (*FloatFn)(float, int, double);

double peer_fold(BinOp op, const double* xs, int n, double init) {
  for (int i = 0; i < n; ++i)
    init = op(init, xs[i]);
  return init;
}
float peer_call_float(FloatFn f) { return f(2.5f, 3, 0.25); }
static double mul(double a, double b) { return a * b; }
static double sub(double a, double b) { return a - b; }
BinOp peer_get_op(int which) { return which == 0 ? mul : sub; }
