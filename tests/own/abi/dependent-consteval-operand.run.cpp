// #209 Itanium C++ ABI §5.1.6. The peer calls the specialization defined in this object.
// PEER: dependent-consteval-operand-peer.cpp
// EXIT: 0
consteval int slot(int key, int padding) { return key + padding; }

template<int Index>
struct Tag { int payload; };

template<int Key>
using Selected = Tag<slot(Key, 3)>;

template<int Key>
int read_slot(const Selected<Key>& item) { return item.payload; }

int peer();

int main() {
  Tag<5> item{19};
  if (read_slot<2>(item) != 19) return 1;
  return peer();
}
