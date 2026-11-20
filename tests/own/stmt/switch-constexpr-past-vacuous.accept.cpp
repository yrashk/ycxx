// [stmt.dcl]/2 and [basic.life]/2: dispatch starts the lifetime of a newly active local.
// XFAIL: gcc GCC 16 rejects assigning to the bypassed local in constant evaluation.
constexpr int switch_past_vacuous(int v) {
  switch (v) {
  case 1:
    int x;
    x = 1;
    return x;
  case 2:
    x = 2;
    return x;
  }
  return 0;
}

static_assert(switch_past_vacuous(1) == 1);
static_assert(switch_past_vacuous(2) == 2);
