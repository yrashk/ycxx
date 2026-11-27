// Local classes (#117), spec-only: a local class of a member function has the same access as the
// member function, so it can use private static members and private member types of the
// enclosing class ([class.access.general], [class.local]).
class Box {
  using Value = int;
  static int secret() { return 40; }
  static constexpr int bonus = 2;
  int held = 0;

public:
  int open() {
    struct Key {
      Value v;
      int turn(Box& b) const { return b.held + v + secret() + bonus; }
    };
    held = 1;
    Key k{3};
    return k.turn(*this);
  }
};

int main() {
  Box b;
  if (b.open() != 1 + 3 + 40 + 2) return 1;
  return 0;
}
