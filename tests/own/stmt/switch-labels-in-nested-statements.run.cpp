// #337 (ι): case labels may appear anywhere in the switch body, including inside nested blocks,
// loops and if statements; the switch jumps into them ([stmt.switch]/2, /5). Duff's device.
// EXIT: 0
int copy(int* to, const int* from, int count) {
  int n = (count + 3) / 4;
  int copied = 0;
  switch (count % 4) {
    case 0: do { *to++ = *from++; ++copied;
    case 3:      *to++ = *from++; ++copied;
    case 2:      *to++ = *from++; ++copied;
    case 1:      *to++ = *from++; ++copied;
            } while (--n > 0);
  }
  return copied;
}

int into_blocks(int v) {
  int r = 0;
  switch (v) {
    {
      case 1: r += 1;
      {
        case 2: r += 10;
      }
    }
    if (v > 100) {
      case 3: r += 100;
    } else {
      case 4: r += 1000;
    }
    break;
    default: r = -1;
  }
  return r;
}

int main() {
  int src[11] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
  for (int count = 1; count <= 11; ++count) {
    int dst[11] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (copy(dst, src, count) != count) return 1;
    for (int i = 0; i < 11; ++i) {
      int expected = i < count ? src[i] : 0;
      if (dst[i] != expected) return 2;
    }
  }
  // case 1 and 2 fall into the if; v <= 100 so its else branch runs.
  if (into_blocks(1) != 1011) return 3;
  if (into_blocks(2) != 1010) return 4;
  // Jumping to case 3 inside the if's then-branch skips the condition and the else branch.
  if (into_blocks(3) != 100) return 5;
  if (into_blocks(4) != 1000) return 6;
  if (into_blocks(5) != -1) return 7;
  return 0;
}
