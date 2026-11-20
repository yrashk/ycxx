// [expr.new], [expr.delete]: initialization, ordinary deallocation, and a typed null.
int main() {
  int* initialized = new int(19);
  int* value_initialized = new int{};
  int* default_initialized = new int;
  *default_initialized = 23;
  int result = *initialized + *value_initialized + *default_initialized;
  delete initialized;
  delete value_initialized;
  delete default_initialized;
  int* empty = nullptr;
  delete empty;
  return result == 42 ? 0 : 1;
}
