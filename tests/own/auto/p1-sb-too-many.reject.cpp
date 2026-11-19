// θ step 1 (#279), spec-only: the number of names in a structured binding declaration must equal the number
// of non-static data members ([dcl.struct.bind]).
struct S {
  int x;
  int y;
};
int main() {
  S s{1, 2};
  auto [a, b, c] = s; //! error
  return 0;
}
