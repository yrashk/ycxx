// θ step 1 (#279), spec-only: a structured binding declaration needs an initializer ([dcl.struct.bind],
// [dcl.pre]).
int main() {
  auto [a, b]; //! error
  return 0;
}
