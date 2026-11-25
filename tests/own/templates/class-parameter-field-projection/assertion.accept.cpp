// #317 control, [temp.inst], [dcl.pre]: a pointer leaves the assertion dormant.
struct Flag {
  bool ok;
};

template<Flag V> struct Checked {
  static_assert(V.ok, "flag must be true");
};

Checked<Flag{true}> demanded;
Checked<Flag{false}>* undemanded;
