// #337 (ι): goto transfers control to a labelled statement before or after it in the function
// ([stmt.goto], [stmt.label]); labels have function scope, so a goto may name a label defined
// later and may jump into and out of nested blocks.
// EXIT: 0
int forward(int v) {
  int r = 0;
  if (v > 0) goto positive;
  r = -1;
  goto done;
positive:
  r = 1;
done:
  return r;
}

int backward() {
  int tries = 0;
again:
  ++tries;
  if (tries < 4) goto again;
  return tries;
}

int out_of_blocks() {
  int r = 0;
  for (int i = 0; i < 10; ++i) {
    for (int j = 0; j < 10; ++j) {
      if (i * j == 12) {
        r = i * 10 + j;
        goto found;
      }
    }
  }
  return -1;
found:
  return r;
}

int into_block(int v) {
  int r = 0;
  if (v) goto inside;
  r = 100;
  {
  inside:
    r += 5;
  }
  return r;
}

int skips_code() {
  goto later;
  return 1;
later:
  return 0;
}

int main() {
  if (forward(5) != 1) return 1;
  if (forward(-5) != -1) return 2;
  if (backward() != 4) return 3;
  if (out_of_blocks() != 26) return 4;
  if (into_block(1) != 5) return 5;
  if (into_block(0) != 105) return 6;
  if (skips_code() != 0) return 7;
  return 0;
}
