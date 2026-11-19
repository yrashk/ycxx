// ι (#291): a block variable with static storage duration and a dynamic initializer is
// initialized the first time control passes through its declaration, and never again
// ([stmt.dcl]/3); its value survives between calls ([basic.stc.static]/1).
// EXIT: 0
int calls = 0;

int init() {
  ++calls;
  return 40;
}

int get() {
  static int v = init();
  return ++v;
}

int counter() {
  static int n = 0;
  return ++n;
}

int main() {
  if (calls != 0) return 1;
  if (get() != 41) return 2;
  if (calls != 1) return 3;
  for (int i = 0; i < 10; i++) get();
  if (calls != 1) return 4;
  if (get() != 52) return 5;

  if (counter() != 1) return 6;
  if (counter() != 2) return 7;
  int last = 0;
  for (int i = 0; i < 5; i++) last = counter();
  if (last != 7) return 8;
  return 0;
}
