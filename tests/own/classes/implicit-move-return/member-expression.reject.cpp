// [expr.prim.id.unqual]: a member access is not a move-eligible id-expression.
struct MoveOnly {
  MoveOnly();
  MoveOnly(const MoveOnly&) = delete;
  MoveOnly(MoveOnly&&);
};

struct Owner {
  MoveOnly member;
};

MoveOnly from_member() {
  Owner local;
  return local.member;  //! error: copy constructor is deleted
}
