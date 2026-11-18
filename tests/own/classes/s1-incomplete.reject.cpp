// δ s1 (#86): a member of the class's own (incomplete) type, an object of a declared but
// undefined class type, member access through a pointer to an incomplete class
// ([class.mem.general], [basic.types.general], [expr.ref]).
struct Self {
  int v;
  Self inner; //! error
};

struct Undefined;

int use(Undefined* u) {
  return u->x; //! error
}

int main() {
  Undefined u; //! error
  return 0;
}
