// [expr.new], [expr.delete]: one-dimensional scalar arrays.
constexpr char* clone(unsigned long length) {
  char* p = new char[length + 1];
  return p;
}

void scalar_elements(short n) {
  int* defaulted = new int[n + 1];
  int* value_initialized = new int[n + 1]();
  const int* list_initialized = new const int[n + 1]{};
  delete[] defaulted;
  delete[] value_initialized;
  delete[] list_initialized;
}

void zero_and_null() {
  unsigned* empty = new unsigned[0]();
  delete[] empty;
  int* null = nullptr;
  delete[] null;
}

void pointer_elements(unsigned n) {
  int** p = ::new int*[n]();
  ::delete[] p;
}

using QualifiedResult = decltype(::new int* const[2]{});
using QualifiedResult = int* const*;
using DeleteResult = decltype(::delete[] static_cast<const int*>(nullptr));
using DeleteResult = void;
