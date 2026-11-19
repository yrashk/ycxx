// #212: a function missing its '}' makes the next function a definition in a block, which is
// ill-formed ([dcl.fct.def.general]/2), not unsupported.
int f() { return 1;
int main() { return 0; } //! error
