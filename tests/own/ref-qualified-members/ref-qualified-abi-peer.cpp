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

int Peer::select(int adjustment) & { return value + adjustment + 20; }
int Peer::select(int adjustment) const && { return value + adjustment + 30; }

int call_native() {
  Native native{8};
  const Native constant{9};
  if (native.select(10) != 18) return 1;
  if (static_cast<const Native&&>(constant).select(11) != 30) return 2;
  return 0;
}
}
