// #337 (ι): jumping forward past a declaration with vacuous initialization is allowed
// ([stmt.dcl]/2, [basic.life]/2), by goto and by a switch.
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

// The fallthrough edge leaves x active, so an earlier pointer still aliases x.
int goto_join(bool jump) {
  int* saved = nullptr;
  if (jump) goto join;
  int x;
  x = 11;
  saved = &x;
join:
  if (jump) {
    x = 17;
    saved = &x;
  }
  if (saved != &x) return -1;
  *saved += 3;
  return x;
}

// Both dispatch and ordinary execution can arrive at the second case.
int switch_join(int v) {
  int* saved = nullptr;
  switch (v) {
  case 1:
    int x;
    x = 23;
    saved = &x;
    [[fallthrough]];
  case 2:
    if (v == 2) {
      x = 29;
      saved = &x;
    }
    if (saved != &x) return -1;
    x += 3;
    return *saved;
  }
  return 0;
}

// The backward jump exits the inner scope. The second entry jumps past x
// while the outer local stays active across the entire sequence.
int scope_reentry() {
  int result = 0;
  int* outer = &result;
  int pass = 0;
again:
  {
    int* saved = nullptr;
    if (pass != 0) goto join;
    int x;
    x = 31;
    saved = &x;
  join:
    if (pass != 0) {
      x = 37;
      saved = &x;
    }
    if (saved != &x || saved == outer) return -1;
    *saved += 2;
    *outer += x;
    if (pass++ == 0) goto again;
  }
  return result;
}

// The jump skips the branch condition and enters the scope of x at the label.
int nested_entry(bool jump, bool take_branch) {
  int result = 0;
  if (jump) goto inside;
  if (take_branch) {
    int x;
    x = 41;
    result += x;
  inside:
    x = 43;
    result += x;
    goto outside;
  }
  result = 47;
outside:
  return result;
}

int main() {
  if (forward_past_vacuous(0) != 5) return 1;
  if (forward_past_vacuous(1) != 7) return 2;
  if (switch_past_vacuous(1) != 1) return 3;
  if (switch_past_vacuous(2) != 2) return 4;
  if (goto_join(false) != 14) return 5;
  if (goto_join(true) != 20) return 6;
  if (switch_join(1) != 26) return 7;
  if (switch_join(2) != 32) return 8;
  if (scope_reentry() != 72) return 9;
  if (nested_entry(true, false) != 43) return 10;
  if (nested_entry(false, true) != 84) return 11;
  if (nested_entry(false, false) != 47) return 12;
  return 0;
}
