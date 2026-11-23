// #917 [expr.type.conv], [temp.variadic].
struct Packet {
  int value;
  Packet() : value(5) {}
  explicit Packet(int first) : value(first) {}
  Packet(int first, int second) : value(first * 10 + second) {}
};

struct Source { using type = Packet; };

template<class Provider, class... Args>
int assemble(Args... args) {
  return typename Provider::type(args...).value;
}

int main() {
  if (assemble<Source>() != 5) return 1;
  if (assemble<Source>(3) != 3) return 2;
  if (assemble<Source>(4, 8) != 48) return 3;
  return 0;
}
