struct S {
  int value;
  S(int n) : value(n) {}
};

int main() {
  S b(S(-1));
  S c(S(+1));
  return b.value == -1 && c.value == 1 ? 0 : 1;
}
