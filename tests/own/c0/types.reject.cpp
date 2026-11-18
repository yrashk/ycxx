// [dcl.type.general], [dcl.init], [basic.types.general], [expr.unary.op]: invalid specifier
// combinations, incomplete types, missing initializers, and operands of the wrong type.
long char wrong; //! error
void nothing; //! error
const int uninitialized; //! error
int* from_int = 1; //! error
int main() {
  int a = 1;
  int b = *a; //! error
  int* c = &3; //! error
  const char* s = "abc";
  char* t = s; //! error
  return a + b;
}
