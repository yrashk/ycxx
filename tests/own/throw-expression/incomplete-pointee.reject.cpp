// [except.throw] A pointer to an incomplete class cannot be thrown.
struct Incomplete;

void raise() {
  throw static_cast<const Incomplete*>(nullptr); //! error
}
