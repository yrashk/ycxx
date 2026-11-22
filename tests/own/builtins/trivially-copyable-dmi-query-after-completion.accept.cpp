// #218: The default member initializer can query an earlier complete class.
struct AtMember;
struct AtMember { int n; };
struct Probe {
  bool value = __is_trivially_copyable(AtMember);
  Probe();
};
Probe::Probe() {}
