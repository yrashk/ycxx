// The other side of abi-interleaved.run.cpp, compiled by Clang.
double peer_inter(int i1, double d1, long i2, float d2, int i3, double d3, char i4, double d4,
                  int i5, float d5, short i6, double d6, int i7, double d7, long long i8,
                  double d8, int i9, double d9, unsigned i10, float d10) {
  double ints = i1 + 2 * i2 + 3 * i3 + 4 * i4 + 5 * i5 + 6 * i6 + 7 * i7 + 8 * i8 + 9 * i9 +
                10.0 * i10;
  double fps = d1 + 2 * d2 + 3 * d3 + 4 * d4 + 5 * d5 + 6 * d6 + 7 * d7 + 8 * d8 + 9 * d9 +
               10 * d10;
  return ints * 1000 + fps;
}
int peer_trunc(double d, int i, float f) { return d * i + f; }

double main_inter(int i1, double d1, long i2, float d2, int i3, double d3, char i4, double d4,
                  int i5, float d5, short i6, double d6, int i7, double d7, long long i8,
                  double d8, int i9, double d9, unsigned i10, float d10);
int main_trunc(double d, int i, float f);

int peer_calls_main() {
  double r = main_inter(2, 0.25, 2, 0.25f, 2, 0.25, 2, 0.25, 2, 0.25f, 2, 0.25, 2, 0.25, 2, 0.25,
                        2, 0.25, 1, 2.0f);
  if (r != 100031.25) return 1; // ints 90 + 10 = 100; floats 45 * 0.25 + 20 = 31.25
  if (main_trunc(2.5, 3, 0.75f) != 8) return 2;
  return 0;
}
