// #740 [expr.cond], [expr.comma], [dcl.init], [class.copy.elision].
// EXIT: 0
struct Value { int n; };

int check(bool first) {
  int decisions = 0;
  Value selected = (++decisions, first) ? Value{19} : Value{31};
  if (decisions != 1 || selected.n != (first ? 19 : 31)) return 1;
  Value sequenced = (++decisions, Value{43});
  return decisions != 2 || sequenced.n != 43;
}

int main() { return check(true) || check(false); }
