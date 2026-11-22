// Native side of the address mapping witness in reinterpret-pointer-integral.run.cpp.
unsigned long peer_object_bits(int* p) { return reinterpret_cast<unsigned long>(p); }
int* peer_object_pointer(unsigned long bits) { return reinterpret_cast<int*>(bits); }
long long peer_void_bits(const volatile void* p) { return reinterpret_cast<long long>(p); }
const volatile void* peer_void_pointer(long long bits) {
  return reinterpret_cast<const volatile void*>(bits);
}
