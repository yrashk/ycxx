// #157 [class.copy.ctor]: a user-declared destructor suppresses the implicit move.
// The rvalue reference member deletes the copy constructor.
// XFAIL: gcc GCC also reports an error at the declaration of the class with the reference.
struct MoveSuppressed {
  int&& reference;
  ~MoveSuppressed() = default;
};

void move_suppressed(MoveSuppressed&& source) {
  MoveSuppressed result(static_cast<MoveSuppressed&&>(source)); //! error
}
