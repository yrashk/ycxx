// [dcl.struct.bind]/1, CWG 2506: a by-value array binding gets its cv from its decl-specifiers.
// XFAIL: clang,gcc both keep the source array's const qualification.
int main() {
  const int source[1][2] = {{1, 2}};
  auto [row] = source;
  row[0] = 3;
  const int* const pointers[1] = {&source[0][0]};
  auto [ptr] = pointers;
  ptr += 1;
  return row[0] != 3 || source[0][0] != 1 || *ptr != 2;
}
