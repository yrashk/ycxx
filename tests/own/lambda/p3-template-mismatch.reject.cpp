// θ step 3 (#281), spec-only: `[]<class T>(T a, T b)` called with an int and a char deduces conflicting
// values for T, so the only candidate is not viable ([temp.deduct.call], [temp.deduct.type],
// [over.call.object]).
int main() {
  auto max = []<class T>(T a, T b) { return a < b ? b : a; };
  return max(1, 'c'); //! error
}
