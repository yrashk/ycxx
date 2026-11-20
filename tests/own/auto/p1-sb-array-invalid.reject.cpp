// [dcl.struct.bind]/5: an initializer cannot refer to one of the newly declared names.
int arr[2];
void self_reference() {
  auto [arr, value] = arr; //! error
}

// [dcl.struct.bind]/2, /6: the array bound determines the number of bindings.
void too_many() {
  auto [x, y, z] = arr; //! error
}
