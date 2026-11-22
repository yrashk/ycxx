// [expr.prim.id.unqual]: a static local object lacks automatic storage duration.
struct MoveOnly {
  MoveOnly();
  MoveOnly(const MoveOnly&) = delete;
  MoveOnly(MoveOnly&&);
};

MoveOnly from_static_local() {
  static MoveOnly local;
  return local;  //! error: copy constructor is deleted
}
