// #337 (ι): a continue statement shall be enclosed by an iteration statement ([stmt.cont]/1);
// a switch statement that is not inside a loop is not enough.
int f(int v) {
  int r = 0;
  switch (v) {
    case 1:
      r = 1;
      continue; //! error
    default:
      r = 2;
  }
  return r;
}

int main() { return f(2) - 2; }
