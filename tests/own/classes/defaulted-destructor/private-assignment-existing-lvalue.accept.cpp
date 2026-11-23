// #157 [expr.assign], [dcl.init.list], [dcl.init.ref].
// The copy-assignment parameter binds to the existing object.
struct Reading {
  int value;
private:
  ~Reading() = default;
};

void assign(Reading& destination, const Reading& source) {
  destination = {source};
}
