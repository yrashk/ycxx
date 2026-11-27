// #1418 [temp.inst]/3, [dcl.init.aggr]/12: a used default member initializer is instantiated for
// the specialization's arguments, and too many clauses for its bound are an error.
template <int N> struct Three { int w[N] = {1, 2, 3}; };  //! error

int main() {
  Three<2> t;
  return t.w[0];
}
