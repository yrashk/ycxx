// δ s1 (#86), from the step's review: an empty list gives an array no bound ([dcl.init.aggr]/12).
int main() {
  int e[] = {}; //! error
  return 0;
}
