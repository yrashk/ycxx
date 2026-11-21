// [except.pre] Intra-block and outward jumps are allowed.
int transfer(int n) {
  for (;;) {
    try {
      if (n == 0) break;
      if (n == 1) goto protected_label;
      if (n == 2) goto finished;
      throw n;
    protected_label:
      return n;
    } catch (int value) {
      if (value == 3) break;
      if (value == 4) goto handler_label;
      if (value == 5) goto finished;
      --n;
      continue;
    handler_label:
      return value;
    }
  }
finished:
  return 0;
}
