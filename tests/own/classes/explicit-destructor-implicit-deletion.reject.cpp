// #722: a member's deleted destructor makes the containing destructor unusable.
// XFAIL: gcc GCC adds a second error at the implicit destructor's declaration.
struct Deleted { ~Deleted() = delete; };
struct ContainsDeleted { Deleted member; };
void destroy(ContainsDeleted& object) {
  object.~ContainsDeleted(); //! error
}
