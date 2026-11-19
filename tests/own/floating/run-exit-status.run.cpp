// [conv.fpint], [basic.start.main]: main's int result computed from floating arithmetic, with no
// output: (0.1 + 0.2) * 100 truncates to 30, and 2.5 * 3 to 7.
// EXIT: 37
double scale(double x, double k) { return x * k; }

int main() {
  double s = 0.1;
  s += 0.2;
  int a = scale(s, 100);
  int b = scale(2.5, 3);
  return a + b;
}
