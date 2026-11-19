// #337 (ι): jumping forward past a declaration with vacuous initialization is allowed
// ([stmt.dcl]/2, [basic.life]/2), by goto and by a switch.
// XFAIL: ycxx function_ir allocates the object where it is declared (#341)
// EXIT: 0
int forward_past_vacuous(int v) {
  if (v) goto skip;
  int x;
  x = 5;
  return x;
skip:
  x = 7;
  return x;
}

int switch_past_vacuous(int v) {
  switch (v) {
  case 1:
    int y;
    y = 1;
    return y;
  case 2:
    y = 2;
    return y;
  }
  return 0;
}

int main() {
  if (forward_past_vacuous(0) != 5) return 1;
  if (forward_past_vacuous(1) != 7) return 2;
  if (switch_past_vacuous(1) != 1) return 3;
  if (switch_past_vacuous(2) != 2) return 4;
  return 0;
}
