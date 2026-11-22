// #209 Itanium C++ ABI §5.1.6. The peer calls specializations defined in this object.
// PEER: dependent-consteval-call-peer.cpp
// EXIT: 0
template<int Code>
struct Tag { int payload; };

template<class T>
consteval int code_for() { return sizeof(T); }

template<class T>
using TagFor = Tag<code_for<T>()>;

template<class T>
int read_tag(const TagFor<T>& tag) { return tag.payload; }

int peer();

int main() {
  Tag<2> short_tag{17};
  Tag<5> long_tag{29};
  if (read_tag<char[2]>(short_tag) != 17) return 1;
  if (read_tag<char[5]>(long_tag) != 29) return 2;
  return peer();
}
