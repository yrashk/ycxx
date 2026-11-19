// ι (#298), from the review: the class's members are used only if lookup finds both `begin` and
// `end` ([stmt.ranged]/1.2, P0962); otherwise the non-member functions are.
// EXIT: 0
int g[3] = {1, 2, 3};
struct OnlyEnd {
  int* end() { return g + 3; }
};
int* begin(OnlyEnd&) { return g; }
int* end(OnlyEnd&) { return g + 2; }
struct DataBegin {
  int begin;
};
int* begin(DataBegin&) { return g + 1; }
int* end(DataBegin&) { return g + 3; }
int main() {
  int s = 0;
  OnlyEnd c;
  for (int x : c) s += x;  // the non-member pair: 1 + 2
  DataBegin d{0};
  for (int x : d) s += x * 10;  // 20 + 30
  return s == 53 ? 0 : 1;
}
