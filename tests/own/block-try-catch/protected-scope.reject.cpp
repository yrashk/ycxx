// [basic.scope.block] The protected block's locals are invisible in the handler.
int f() {
  try {
    int local = 1;
    throw local;
  } catch (...) {
    return local; //! error
  }
}
