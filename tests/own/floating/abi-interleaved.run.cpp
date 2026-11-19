// [expr.call], [dcl.fct]: integer and floating arguments interleaved, more than eight of each, so
// each kind fills its own registers (x0-x7, v0-v7) and then the stack, across objects in both
// directions; and a float, an int and a double result.
// PEER: abi-interleaved-peer.cpp
// EXIT: 0
double peer_inter(int i1, double d1, long i2, float d2, int i3, double d3, char i4, double d4,
                  int i5, float d5, short i6, double d6, int i7, double d7, long long i8,
                  double d8, int i9, double d9, unsigned i10, float d10);
int peer_trunc(double d, int i, float f);
int peer_calls_main();

double main_inter(int i1, double d1, long i2, float d2, int i3, double d3, char i4, double d4,
                  int i5, float d5, short i6, double d6, int i7, double d7, long long i8,
                  double d8, int i9, double d9, unsigned i10, float d10) {
  double ints = i1 + 2 * i2 + 3 * i3 + 4 * i4 + 5 * i5 + 6 * i6 + 7 * i7 + 8 * i8 + 9 * i9 +
                10.0 * i10;
  double fps = d1 + 2 * d2 + 3 * d3 + 4 * d4 + 5 * d5 + 6 * d6 + 7 * d7 + 8 * d8 + 9 * d9 +
               10 * d10;
  return ints * 1000 + fps;
}
int main_trunc(double d, int i, float f) { return d * i + f; }

int main() {
  // ints all 1 except i10 = 2 (65); floats all 0.5 except d10 = 1 (32.5).
  double r = peer_inter(1, 0.5, 1, 0.5f, 1, 0.5, 1, 0.5, 1, 0.5f, 1, 0.5, 1, 0.5, 1, 0.5, 1, 0.5,
                        2, 1.0f);
  if (r != 65032.5) return 1;
  if (peer_trunc(2.5, 3, 0.75f) != 8) return 2;
  int p = peer_calls_main();
  return p == 0 ? 0 : 10 + p;
}
