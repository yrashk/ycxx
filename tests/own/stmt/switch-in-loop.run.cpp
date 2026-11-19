// #337 (ι): break inside a switch inside a loop leaves only the switch ([stmt.break]); continue
// inside a switch inside a loop continues the loop ([stmt.cont]); a break after the switch
// leaves the loop.
// EXIT: 0
int main() {
  int sum = 0;
  int iterations = 0;
  for (int i = 0; i < 6; ++i) {
    ++iterations;
    switch (i) {
      case 1: sum += 1; break;
      case 2: continue;
      case 3: sum += 3;
      case 4: sum += 4; break;
      default: sum += 100;
    }
    sum += 1000;
  }
  // i=0: 100+1000, 1: 1+1000, 2: skip, 3: 3+4+1000, 4: 4+1000, 5: 100+1000
  if (sum != 5212) return 1;
  if (iterations != 6) return 2;

  int n = 0;
  int seen = 0;
  while (true) {
    switch (n) {
      case 5: break;
      default: ++n; ++seen; continue;
    }
    break;
  }
  if (n != 5) return 3;
  if (seen != 5) return 4;

  // continue in a do statement goes to the loop condition.
  int k = 0;
  int odd = 0;
  do {
    switch (k % 2) {
      case 0: ++k; continue;
      case 1: ++odd; break;
    }
    ++k;
  } while (k < 10);
  if (odd != 5) return 5;
  if (k != 10) return 6;
  return 0;
}
