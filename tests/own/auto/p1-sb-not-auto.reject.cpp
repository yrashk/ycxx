// θ step 1 (#279), spec-only: the decl-specifier-seq of a structured binding declaration contains the type
// specifier `auto`, not a concrete type ([dcl.struct.bind], [dcl.pre]).
int main() {
  int arr[2] = {1, 2};
  int [a, b] = arr; //! error
  return 0;
}
