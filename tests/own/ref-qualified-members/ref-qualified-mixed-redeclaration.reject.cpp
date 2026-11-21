// XFAIL: gcc GCC 16 accepts a redeclaration that [basic.scope.scope]/3.1 makes corresponding.
struct Box {
  int select() { return 1; }
  int select() & { return 2; } //! error
};
