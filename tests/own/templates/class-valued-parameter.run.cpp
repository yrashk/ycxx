// #317, [temp.param]/11-13, [temp.arg.nontype]/1,4, [temp.type]/1,2:
// equivalent concrete aggregate values select one specialization, and its members read the parameter.
// EXIT: 0
struct Options {
  int count;
  bool enabled = true;
};

constexpr Options named{42, true};

template<Options V> struct Read {
  static constexpr int count = V.count;
  static constexpr bool enabled = V.enabled;
  int run() const { return V.enabled ? V.count : -V.count; }
};

static_assert(Read<Options{}>::count == 0);
static_assert(Read<Options{}>::enabled);
static_assert(Read<Options{6 * 7}>::count == 42);
static_assert(Read<Options{6 * 7}>::enabled);
static_assert(!Read<Options{42, false}>::enabled);
static_assert(Read<named>::count == 42 && Read<named>::enabled);

int main() {
  Read<named> equivalent = Read<Options{6 * 7}>{};
  return equivalent.run() == 42 && Read<Options{42, false}>{}.run() == -42 ? 0 : 1;
}
