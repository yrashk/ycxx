struct InnerType { using Value = int; };
class Outer { using Inner = InnerType; };

int parens() { return Outer::Inner::Value(5); } //! error
int braces() { return Outer::Inner::Value{5}; } //! error
