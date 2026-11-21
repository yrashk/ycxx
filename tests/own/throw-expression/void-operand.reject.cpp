// [except.throw] A void expression cannot form an exception object.
void nothing();

void raise() {
  throw nothing(); //! error
}
