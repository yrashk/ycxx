// PEER: ref-qualified-abi-peer.cpp
// EXIT: 0
namespace fixture {
struct Native {
  int value;
  int select(int adjustment) &;
  int select(int adjustment) const &&;
};

struct Peer {
  int value;
  int select(int adjustment) &;
  int select(int adjustment) const &&;
};

int Native::select(int adjustment) & { return value + adjustment; }
int Native::select(int adjustment) const && { return value + adjustment + 10; }
int call_native();
}

int main() {
  fixture::Peer peer{4};
  const fixture::Peer constant{5};
  if (peer.select(6) != 30) return 1;
  if (static_cast<const fixture::Peer&&>(constant).select(7) != 42) return 2;
  if (fixture::call_native() != 0) return 3;
  return 0;
}
