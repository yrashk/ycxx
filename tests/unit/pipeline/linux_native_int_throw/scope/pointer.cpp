// Valid C++ throw of a pointer value.
extern "C" void authored_pointer(int* address) {
  throw address;
}
