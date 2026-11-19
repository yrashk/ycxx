// [conv.func], [expr.call]: pointers to functions with float and double parameters and results,
// passed across objects: the peer calls the test's functions through them, and the test calls
// the peer's.
// XFAIL: ycxx calls through a function pointer (#196)
// PEER: abi-callbacks-peer.cpp
// EXIT: 0
typedef double (*BinOp)(double, double);
typedef float (*FloatFn)(float, int, double);

double peer_fold(BinOp op, const double* xs, int n, double init);
float peer_call_float(FloatFn f);
BinOp peer_get_op(int which);

double main_max(double a, double b) { return a > b ? a : b; }
double main_add(double a, double b) { return a + b; }
float main_combine(float f, int i, double d) { return f * i + d; }

int main() {
  double xs[4] = {1.5, -2, 4.25, 0.5};
  if (peer_fold(main_add, xs, 4, 0) != 4.25) return 1;
  if (peer_fold(main_max, xs, 4, -100) != 4.25) return 2;
  if (peer_call_float(main_combine) != 7.75f) return 3;
  BinOp mul = peer_get_op(0);
  BinOp sub = peer_get_op(1);
  if (mul(1.5, -4) != -6 || sub(0.5, 0.25) != 0.25) return 4;
  return 0;
}
