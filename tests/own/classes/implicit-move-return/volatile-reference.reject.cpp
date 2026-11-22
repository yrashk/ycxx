// [expr.prim.id.unqual]: a reference to a volatile object is not move-eligible.
struct MoveOnly {
  MoveOnly(volatile MoveOnly&) = delete;
  MoveOnly(volatile MoveOnly&&);
};

using Volatile = volatile MoveOnly;

MoveOnly from_volatile_reference(Volatile&& parameter) {
  return parameter;  //! error: volatile lvalue copy constructor is deleted
}
