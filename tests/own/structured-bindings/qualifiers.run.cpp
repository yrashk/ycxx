// #285, spec-only. The object supplies cv to ordinary members, and reference members keep the declared type ([dcl.struct.bind]).
template<class Actual, class Expected>
constexpr bool same_type = false;

template<class Type>
constexpr bool same_type<Type, Type> = true;

struct Controls {
  int fixed;
  volatile short sensor;
  int& link;
  int&& endpoint;
};

struct MoveReference { int&& endpoint; };
struct MoveOuter { MoveReference inner; };

struct CopyReference {
  int&& endpoint;
  CopyReference(int&& value) : endpoint(static_cast<int&&>(value)) {}
  CopyReference(const CopyReference& other) : endpoint(static_cast<int&&>(other.endpoint)) {}
};

int main() {
  int linked = 7;
  int end = 11;
  Controls controls{5, 13, linked, static_cast<int&&>(end)};
  const auto& [fixed, sensor, link, endpoint] = controls;
  static_assert(same_type<decltype(fixed), const int>);
  static_assert(same_type<decltype((fixed)), const int&>);
  static_assert(same_type<decltype(sensor), const volatile short>);
  static_assert(same_type<decltype((sensor)), const volatile short&>);
  static_assert(same_type<decltype(link), int&>);
  static_assert(same_type<decltype((link)), int&>);
  static_assert(same_type<decltype(endpoint), int&&>);
  static_assert(same_type<decltype((endpoint)), int&>);

  link = 17;
  endpoint = 19;
  if (linked != 17 || end != 19 || fixed != 5 || sensor != 13) return 1;

  volatile Controls live{23, 29, linked, static_cast<int&&>(end)};
  auto& [value, meter, attached, terminal] = live;
  static_assert(same_type<decltype(value), volatile int>);
  static_assert(same_type<decltype((value)), volatile int&>);
  static_assert(same_type<decltype(meter), volatile short>);
  static_assert(same_type<decltype(attached), int&>);
  static_assert(same_type<decltype(terminal), int&&>);
  value = 31;
  meter = 37;
  if (live.fixed != 31 || live.sensor != 37 || attached != 17 || terminal != 19) return 2;

  MoveReference moving{static_cast<int&&>(end)};
  auto [moved] = static_cast<MoveReference&&>(moving);
  CopyReference copying(static_cast<int&&>(linked));
  auto [copied] = copying;
  if (&moved != &end || &copied != &linked) return 3;

  MoveOuter outer{{static_cast<int&&>(end)}};
  auto [inner] = static_cast<MoveOuter&&>(outer);
  if (&inner.endpoint != &end) return 4;
}
