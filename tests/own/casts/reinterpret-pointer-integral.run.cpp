// #199, [expr.reinterpret.cast]: compare each direction separately with the native mapping.
// Use live interior pointers so the check also observes high address bits and element offsets.
// PEER: reinterpret-pointer-integral-peer.cpp
// EXIT: 0
unsigned long peer_object_bits(int*);
int* peer_object_pointer(unsigned long);
long long peer_void_bits(const volatile void*);
const volatile void* peer_void_pointer(long long);

int* counted_pointer(int* p, int& calls) {
  ++calls;
  return p;
}

unsigned long counted_bits(unsigned long bits, int& calls) {
  ++calls;
  return bits;
}

int main() {
  int items[3] = {7, 31, 54};
  int* interior = items + 1;
  int pointer_calls = 0;
  unsigned long bits = reinterpret_cast<unsigned long>(counted_pointer(interior, pointer_calls));
  if (pointer_calls != 1 || bits != peer_object_bits(interior)) return 1;
  if (peer_object_pointer(bits) != interior) return 2;

  int integer_calls = 0;
  int* restored = reinterpret_cast<int*>(counted_bits(peer_object_bits(interior), integer_calls));
  if (integer_calls != 1 || restored != interior || *restored != 31) return 3;
  *restored = 62;
  if (items[1] != 62) return 4;

  const volatile void* qualified = items + 2;
  long long qualified_bits = reinterpret_cast<long long>(qualified);
  if (qualified_bits != peer_void_bits(qualified)) return 5;
  if (peer_void_pointer(qualified_bits) != qualified) return 6;
  if (reinterpret_cast<const volatile void*>(peer_void_bits(qualified)) != qualified) return 7;
  return 0;
}
