// [expr.call], [dcl.fct]: more floating arguments than the eight floating argument registers
// (AAPCS64 v0-v7), across objects in both directions: ten doubles, eleven floats, and floats and
// doubles alternating; each argument is weighted by its position.
// PEER: abi-many-floats-peer.cpp
// EXIT: 0
double peer_d10(double a, double b, double c, double d, double e, double f, double g, double h,
                double i, double j);
float peer_f11(float a, float b, float c, float d, float e, float f, float g, float h, float i,
               float j, float k);
double peer_alt(float a, double b, float c, double d, float e, double f, float g, double h,
                float i, double j, float k, double l);
int peer_calls_main();

double main_d10(double a, double b, double c, double d, double e, double f, double g, double h,
                double i, double j) {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j;
}
float main_f11(float a, float b, float c, float d, float e, float f, float g, float h, float i,
               float j, float k) {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j + 11 * k;
}
double main_alt(float a, double b, float c, double d, float e, double f, float g, double h,
                float i, double j, float k, double l) {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j + 11 * k +
         12 * l;
}

int main() {
  // With all arguments 1 except the last, which is 2: the sum of weights plus the last weight.
  if (peer_d10(1, 1, 1, 1, 1, 1, 1, 1, 1, 2) != 65) return 1;
  if (peer_f11(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2) != 77) return 2;
  if (peer_alt(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2) != 90) return 3;
  if (peer_d10(0, 0, 0, 0, 0, 0, 0, 0, 0.5, 0.25) != 7) return 4;
  int r = peer_calls_main();
  return r == 0 ? 0 : 10 + r;
}
