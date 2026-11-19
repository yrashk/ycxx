extern "C" int verify_promotions(int tag, ...);
extern "C" int verify_no_tail(int tag, ...);

int main() {
  unsigned char byte = 251;
  short small = -1223;
  bool truth = true;
  if (int result = verify_promotions(13, byte, small, truth, -(1L << 39), "x", -42, 1L << 41, 3))
    return result;
  return verify_no_tail(29);
}
