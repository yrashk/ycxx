// EXIT: 0
// [class.nest], [temp.dep.type], [dcl.align], [dcl.array]
template<unsigned Len, unsigned Align>
struct Storage {
  struct type {
    alignas(Align) unsigned char data[Len];
  };
};

template<unsigned L, unsigned A>
using StorageType = typename Storage<L, A>::type;

int main() {
  StorageType<3, 8> entries[2];
  entries[0].data[2] = 17;
  entries[1].data[0] = 23;
  if (entries[0].data[2] != 17 || entries[1].data[0] != 23) return 1;

  // The supported native targets use LP64.
  unsigned long first = reinterpret_cast<unsigned long>(&entries[0]);
  unsigned long next = reinterpret_cast<unsigned long>(&entries[1]);
  unsigned long payload = reinterpret_cast<unsigned long>(&entries[0].data[0]);
  if (payload != first) return 2;
  if (next - first != 8) return 3;
  if (first % 8 != 0) return 4;
  return 0;
}
