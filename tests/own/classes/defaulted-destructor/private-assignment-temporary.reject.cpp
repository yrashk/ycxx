// #157 [expr.assign], [dcl.init.list], [class.dtor].
// The copy-assignment parameter binds to a temporary constructed from the list.
struct Reading {
  int value;
private:
  ~Reading() = default;
};

void assign(Reading& destination) {
  destination = {42}; //! error
}
