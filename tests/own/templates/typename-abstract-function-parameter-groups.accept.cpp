// [dcl.fct], [dcl.name], [dcl.ambig.res]
using Return = int;
using Arg = int;
int argument(Arg* value) { return *value; }
int target(int (*function)(Arg*)) {
  Arg value = 17;
  return function(&value);
}
int main() {
  int (*one)(typename ::Return (Arg*)) = target;
  int (*two)(typename ::Return ((Arg*))) = target;
  int (*three)(typename ::Return (((Arg*)))) = target;
  return one(argument) != 17 || two(argument) != 17 || three(argument) != 17;
}
