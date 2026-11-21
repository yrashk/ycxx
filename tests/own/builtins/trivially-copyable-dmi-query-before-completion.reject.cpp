// #218: A later definition of AtMember does not complete it at the default member initializer.
struct AtMember;
struct Probe {
  bool value = __is_trivially_copyable(AtMember); //! error
  Probe();
};
struct AtMember { int n; };
Probe::Probe() {}
