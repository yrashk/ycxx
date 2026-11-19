// #337 (ι): the condition undergoes integral promotions and the case values are converted to the
// promoted type ([stmt.switch]/2, /5, [conv.prom]): bool, unsigned char, short and char
// conditions compare as int.
// EXIT: 0
int on_bool(bool b) {
  switch (b) {
    case 0: return 10;
    case 1: return 11;
  }
  return -1;
}

int on_uchar(unsigned char c) {
  switch (c) {
    case 200: return 1;
    case 255: return 2;
    case 0: return 3;
  }
  return -1;
}

int on_short(short s) {
  switch (s) {
    case -30000: return 1;
    case 30000: return 2;
    case 'x': return 3;
  }
  return -1;
}

int on_char_arith(char c) {
  switch (c + 1) {
    case 'b': return 1;
    case 256: return 2;
  }
  return -1;
}

int main() {
  if (on_bool(false) != 10) return 1;
  if (on_bool(true) != 11) return 2;
  unsigned char big = 200;
  if (on_uchar(big) != 1) return 3;
  if (on_uchar(255) != 2) return 4;
  if (on_uchar(0) != 3) return 5;
  if (on_uchar(1) != -1) return 6;
  short neg = -30000;
  if (on_short(neg) != 1) return 7;
  if (on_short(30000) != 2) return 8;
  if (on_short('x') != 3) return 9;
  if (on_char_arith('a') != 1) return 10;
  if (on_char_arith('c') != -1) return 11;
  return 0;
}
