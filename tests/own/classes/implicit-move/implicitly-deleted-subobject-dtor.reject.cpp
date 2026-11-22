// XFAIL: gcc GCC also reports the deletion chain at the class definitions.
// [class.copy.ctor] A defaulted move is deleted when a potentially constructed
// subobject has a deleted or inaccessible destructor.
// [class.dtor] A defaulted destructor is deleted when destruction of a
// potentially constructed class subobject would call an inaccessible destructor.
class PrivateCleanup {
 public:
  PrivateCleanup(PrivateCleanup&&) noexcept;

 private:
  ~PrivateCleanup() noexcept;
};

struct Member {
  Member(Member&&) noexcept;
  Member(const Member&) = delete;
  PrivateCleanup cleanup;
};

struct Owner {
  Member member;
};

void relocate(Owner&& source) {
  Owner destination(static_cast<Owner&&>(source));  //! error: unusable subobject destructor deletes the move
}
