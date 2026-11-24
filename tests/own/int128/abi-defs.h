// Included by both sides: their definitions, and the checks that call a set of them.
u128 OURS(first)(long a, u128 b) { return b + a; }
u128 OURS(second)(u128 a, u128 b, long c, u128 d) { return a * 3 + b * 5 + c * 7 + d * 11; }
u128 OURS(split)(long a, long b, long c, long d, long e, u128 f, long g, long h) {
  return f + a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * g + 7 * h;
}
u128 OURS(stack)(long a, long b, long c, long d, long e, long f, long g, u128 h, long i, u128 j) {
  return h + a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * i + j * 9;
}
u128 OURS(packed)(long a, long b, long c, long d, long e, long f, long g, long h, signed char i, u128 j,
                  short k, u128 l, bool m) {
  return j + a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * k + l * 11 + m;
}
u128 OURS(mixed)(double a, u128 b, float c, u128 d, double e, u128 f, int g, u128 h) {
  return (long)a + 2 * b + 3 * (long)c + 4 * d + 5 * (long)e + 6 * f + 7 * g + 8 * h;
}
u128 OURS(box)(int a, Box b, long c) { return b.v + a + 2 * c; }
u128 OURS(box_stack)(long a, long b, long c, long d, long e, long f, long g, Box h, long i, Box j) {
  return h.v + a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * i + j.v * 9;
}
Box OURS(box_result)(Box a, i128 b) { return {a.v - (u128)b}; }
i128 OURS(signed)(i128 a, int b, i128 c) { return a * b - c; }
u128 OURS(many)(u128 a, u128 b, u128 c, u128 d, u128 e, u128 f) { return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f; }

template <class F, class G, class H, class I, class J, class K, class L, class M, class N, class O, class P>
int check_calls(F first, G second, H split, I stack, J packed, K mixed, L box, M box_stack, N box_result, O signed_,
                P many) {
  u128 x = wide(0x1111222233334444, 0x5555666677778888), y = wide(0x9999aaaabbbbcccc, 0xddddeeeeffff0000);
  if (first(7, x) != x + 7) return 1;
  if (second(x, y, 9, x ^ y) != x * 3 + y * 5 + 63 + (x ^ y) * 11) return 2;
  if (split(1, 2, 3, 4, 5, x, 6, 7) != x + 1 + 4 + 9 + 16 + 25 + 36 + 49) return 3;
  if (stack(1, 2, 3, 4, 5, 6, 7, x, 8, y) != x + 140 + 64 + y * 9) return 4;
  if (packed(1, 2, 3, 4, 5, 6, 7, 8, -9, x, -10, y, true) != x + 204 - 81 - 100 + y * 11 + 1) return 5;
  if (mixed(1, x, 2, y, 3, x + y, 4, x - y) != 1 + 2 * x + 6 + 4 * y + 15 + 6 * (x + y) + 28 + 8 * (x - y)) return 6;
  if (box(-3, Box{x}, 4) != x - 3 + 8) return 7;
  if (box_stack(1, 2, 3, 4, 5, 6, 7, Box{x}, 8, Box{y}) != x + 140 + 64 + y * 9) return 8;
  if (box_result(Box{x}, (i128)y).v != x - y) return 9;
  if (signed_((i128)-5, -3, (i128)x) != 15 - (i128)x) return 10;
  if (many(x, y, x + 1, y + 1, x + 2, y + 2) != x + 2 * y + 3 * (x + 1) + 4 * (y + 1) + 5 * (x + 2) + 6 * (y + 2)) return 11;
  return 0;
}
