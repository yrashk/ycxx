// #337 (ι): a label shall not be declared twice in the same function ([stmt.label]/1), even in
// different blocks, since labels have function scope.
int f(int v) {
  {
  here:
    ++v;
  }
  {
  here: //! error
    --v;
  }
  return v;
}

int main() { return f(0); }
