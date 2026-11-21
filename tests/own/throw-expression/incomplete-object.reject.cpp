// [except.throw] An exception object must have complete type.
struct Incomplete;

void raise(Incomplete& value) {
  throw value; //! error
}
