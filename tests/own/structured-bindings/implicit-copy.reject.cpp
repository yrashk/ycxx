// [dcl.struct.bind], [class.copy.ctor]: an rvalue-reference member deletes the implicit copy.
// GCC diagnoses the deleted definition at the class as well as the first use.
struct Value { int&& member; }; void copy(Value& x) { auto [member] = x; } //! error

void direct(Value& source) {
  auto [member](source); //! error
}

void list(Value& source) {
  auto [member]{source}; //! error
}

void from_const_xvalue(const Value& source) {
  auto [member] = static_cast<const Value&&>(source); //! error
}

struct NoMove { int&& member; ~NoMove() {} }; void move(NoMove& x) { auto [member] = static_cast<NoMove&&>(x); } //! error
