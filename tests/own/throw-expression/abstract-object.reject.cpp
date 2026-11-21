// [except.throw] An abstract class cannot be the type of an exception object.
struct Abstract { virtual void f() = 0; };

void raise(Abstract& value) {
  throw value; //! error
}
