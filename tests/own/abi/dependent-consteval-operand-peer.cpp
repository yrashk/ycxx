// The peer defines slot for substitution and only declares read_slot.
consteval int slot(int key, int padding) { return key + padding; }

template<int Index>
struct Tag { int payload; };

template<int Key>
using Selected = Tag<slot(Key, 3)>;

template<int Key>
int read_slot(const Selected<Key>& item);

int peer() {
  Tag<5> item{23};
  return read_slot<2>(item) != 23;
}
