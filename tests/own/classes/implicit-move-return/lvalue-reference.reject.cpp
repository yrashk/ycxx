// [expr.prim.id.unqual]: an lvalue reference parameter is not move-eligible.
struct MoveOnly {
  MoveOnly(const MoveOnly&) = delete;
  MoveOnly(MoveOnly&&);
};

MoveOnly from_lvalue_reference(MoveOnly& parameter) {
  return parameter;  //! error: copy constructor is deleted
}
