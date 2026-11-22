// [expr.prim.id.general], [class.mfct.non.static]
struct Box {
  int field;
  static constexpr int value = Box::field; //! error
};
