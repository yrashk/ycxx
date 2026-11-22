// Indirect calls with floating arguments and results, compiled by ycxx (#196, #639).
extern "C" {

double fp196_apply(double (*f)(double, float, int), double x, float y, int z) { return f(x, y, z); }

float fp196_applyf(float (*f)(int, float, long, float), float a, float b) { return f(7, a, -3, b); }

// Eight SSE arguments fill XMM0-XMM7; the ninth goes on the stack.
double fp196_many(double (*f)(double, double, double, double, double, double, double, double, double)) {
  return f(1, 2, 3, 4, 5, 6, 7, 8, 9.5);
}

// Through a variadic pointer, AL must count the vector registers or the callee's va_start
// does not save them (AMD64 psABI §3.2.3, §3.5.7).
double fp196_variadic(double (*f)(int, ...), double a, float b) { return f(3, a, b, 2.5); }

int fp196_variadic_ints(int (*f)(int, ...)) { return f(2, 40, 2); }

// A float on the stack, and AL at its limit of 8 with doubles spilling.
float fp196_floats(float (*f)(float, float, float, float, float, float, float, float, float)) {
  return f(1, 2, 3, 4, 5, 6, 7, 8, 9.5f);
}
double fp196_ten(double (*f)(int, ...)) { return f(10, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 1.5); }

}
