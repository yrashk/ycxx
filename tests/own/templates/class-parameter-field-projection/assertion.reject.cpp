// #317 control, [temp.inst], [dcl.pre]: completing the class checks the assertion.
struct Flag {
  bool ok;
};

template<Flag V> struct Checked {
  static_assert(V.ok, "flag must be true"); //! error
};

Checked<Flag{false}> demanded;
