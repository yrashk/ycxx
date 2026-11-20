// #354: the earlier parameter hides the later static member, and a parameter
// cannot appear as a potentially evaluated expression in a default ([dcl.fct.default]).
struct S {
  int get(int value, int n = value) { return n; } //! error
  static int value;
};
