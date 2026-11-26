// [dcl.fct], [temp.inst]; Itanium ref-qualifier mangling of class template members.
// PEER: template-abi-peer.cpp
// EXIT: 0
namespace fixture {
template <class T>
struct Native {
  T value;
  int select(int adjustment) & { return value + adjustment; }
  int select(int adjustment) const && { return value + adjustment + 10; }
};

template <class T>
struct Peer {
  T value;
  int select(int adjustment) &;
  int select(int adjustment) const &&;
  template <class U>
  int pick(U adjustment) &;
  template <class U>
  int pick(U adjustment) &&;
};
int call_native();
}

int main() {
  fixture::Native<int> native{1};
  const fixture::Native<int> constant{2};
  if (native.select(3) != 4) return 1;
  if (static_cast<const fixture::Native<int>&&>(constant).select(4) != 16) return 2;
  fixture::Peer<int> peer{4};
  const fixture::Peer<int> constant_peer{5};
  if (peer.select(6) != 30) return 3;
  if (static_cast<const fixture::Peer<int>&&>(constant_peer).select(7) != 42) return 4;
  if (fixture::call_native() != 0) return 5;
  if (peer.pick(1L) != 45) return 6;
  if (fixture::Peer<int>{2}.pick(3L) != 55) return 7;
  return 0;
}
