// A narrowing conversion in list-initialization is ill-formed ([dcl.init.list]/3.9): every
// compiler must reject it.
// DIFF: verdict
int main() {
  double d = 1.5;
  int i{d};
  return i;
}
