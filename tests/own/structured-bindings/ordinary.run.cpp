// #285, spec-only. Copy, direct and direct-list initialization evaluate the source once ([dcl.struct.bind]).
struct Reading {
  int low;
  int high;
};

int loads;

Reading& load(Reading& reading) {
  ++loads;
  return reading;
}

struct ExplicitCopy {
  int value;
  explicit ExplicitCopy(int initial) : value(initial) {}
  explicit ExplicitCopy(const ExplicitCopy& source) : value(source.value + 1) {}
};

int main() {
  Reading reading{17, 29};

  auto [low, high] = load(reading);
  if (loads != 1 || low != 17 || high != 29) return 1;
  low = 41;
  high = 43;
  if (reading.low != 17 || reading.high != 29) return 2;

  auto& [left, right](load(reading));
  if (loads != 2 || &left != &reading.low || &right != &reading.high) return 3;
  left = 31;
  reading.high = 47;
  if (reading.low != 31 || right != 47 || low != 41 || high != 43) return 4;

  auto&& [forward_low, forward_high]{load(reading)};
  if (loads != 3 || &forward_low != &left || &forward_high != &right) return 5;
  forward_high = 53;
  if (reading.high != 53) return 6;

  auto [list_low, list_high]{load(reading)};
  if (loads != 4 || list_low != 31 || list_high != 53) return 7;
  list_high = 59;
  if (reading.high != 53 || list_high != 59 || loads != 4) return 8;

  ExplicitCopy initial(61);
  auto [direct_value](initial);
  auto [list_value]{initial};
  if (direct_value != 62 || list_value != 62 || initial.value != 61) return 9;
}
