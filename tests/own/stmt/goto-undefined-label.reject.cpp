// #337 (ι): the identifier in a goto statement shall name a label in the current function
// ([stmt.goto]/1); a label in another function does not count.
void other() {
missing:
  return;
}

int f(int v) {
  if (v) goto missing; //! error
  return 0;
}

int main() { return f(0); }
