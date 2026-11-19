// ι (#298): an array of unknown bound as the range is ill-formed: the array form of the
// end-expr is `__range + __bound`, and there is no bound ([stmt.ranged]/1.1).
extern int u[];

int main() {
  int s = 0;
  for (int x : u) s += x; //! error
  return s;
}
