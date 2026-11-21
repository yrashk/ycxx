// θ step 1 (#279), spec-only: names bound by `const auto& [a, b]` refer to const objects and cannot be
// assigned ([dcl.struct.bind], [expr.assign]).
struct S {
  int a;
  int b;
};
int main() {
  S s{1, 2};
  const auto& [a, b] = s;
  a = 3; //! error
  return b;
}
