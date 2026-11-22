// [class.temporary], [intro.execution] Both markers outlive the call that receives them.
// The later marker's destructor can still read the intervening scalar argument temporary.
// EXIT: 0
int order;
int observed;
const int* selected;

struct Marker {
  int digit;
  explicit Marker(int d) noexcept : digit(d) {}
  ~Marker() noexcept {
    order = order * 10 + digit;
    if (digit == 2) observed = *selected;
  }
};

void use(const Marker&) noexcept {}
const int& hold(const int& v) noexcept { return v; }

bool check(bool first) {
  order = 0;
  observed = 0;
  int value = (use(Marker(1)), selected = first ? &hold(17) : &hold(41),
               use(Marker(2)), *selected);
  return value == (first ? 17 : 41) && order == 21 && observed == value;
}

int main() { return check(true) && check(false) ? 0 : 1; }
