// [basic.start.dynamic], [expr.new]: a namespace-scope initializer allocates before main.
namespace N {
int* value = new int(41);
}
int main() {
  if (*N::value != 41) return 5;
  delete N::value;
  return 0;
}
