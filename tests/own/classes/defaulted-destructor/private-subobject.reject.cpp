// #157 [class.dtor]: an inaccessible member destructor deletes the enclosing destructor.
// XFAIL: gcc GCC also reports an error at the declaration of the enclosing class.
struct Hidden {
private:
  ~Hidden() = default;
};

struct Owner {
  Hidden member;
};

void destroy_owner(Owner& object) {
  object.~Owner(); //! error
}
