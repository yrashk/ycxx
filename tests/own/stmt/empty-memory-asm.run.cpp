// [dcl.asm]/1, GNU extended asm memory clobber
int update(bool checkpoint, int& before, int& after) {
  before += 2;
  if (checkpoint) {
    asm volatile("" ::: "memory");
    after += before;
  } else {
    after -= before;
  }
  return after;
}

int main() {
  int before = 3;
  int after = 10;
  int selected = update(true, before, after);
  bool selected_ok = selected == 15 && before == 5 && after == 15;

  int skipped = update(false, before, after);
  bool skipped_ok = skipped == 8 && before == 7 && after == 8;
  return selected_ok && skipped_ok ? 0 : 1;
}
