// [basic.scope.block] The handler's outermost block cannot redeclare its parameter.
void work();
void f() {
  try { work(); }
  catch (int value) {
    int value = 1; //! error
    (void)value;
  }
}
