// Valid C++ throw of a different scalar type.
extern "C" void authored_unsigned(unsigned value) {
  throw value;
}
