// ζ f1 (#88): the argument for a non-type template parameter must be a constant expression; a
// non-const local variable is not ([temp.arg.nontype], [expr.const]).
template<int N>
int times(int x) { return x * N; }

int main() {
  int n = 3;
  return times<n>(2); //! error
}
