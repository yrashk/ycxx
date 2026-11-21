// [dcl.fct.default] An abstract function-pointer declarator cannot declare a default argument.
// XFAIL: clang Clang 23 accepts the default argument in this exception declaration.
void f() {
  try {}
  catch (int (*)(int = 0)) {} //! error
}
