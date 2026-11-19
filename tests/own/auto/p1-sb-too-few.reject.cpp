// θ step 1 (#279), spec-only: the number of names in a structured binding declaration must equal the array's
// element count ([dcl.struct.bind]).
int main() {
  int arr[3] = {1, 2, 3};
  auto [a, b] = arr; //! error
  return 0;
}
