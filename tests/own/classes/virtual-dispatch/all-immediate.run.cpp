// #1097 [class.virtual], [class.abstract]: a pure immediate final overrider makes a class abstract.
// EXIT: 0

struct Abstract {
  virtual consteval int value() const = 0;
};

struct StillAbstract : Abstract {};

struct Concrete : Abstract {
  consteval int value() const override { return 7; }
};

template<class T>
concept BraceConstructible = requires { T{}; };

static_assert(!BraceConstructible<Abstract>);
static_assert(!BraceConstructible<StillAbstract>);
static_assert(BraceConstructible<Concrete>);

const Abstract* base_address(const Abstract& object) { return &object; }

int main() {
  Concrete object;
  return base_address(object) == static_cast<const Abstract*>(&object) ? 0 : 1;
}
