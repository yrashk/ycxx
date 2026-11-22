// [expr.throw] Arrays and functions decay before determining the exception object type.
extern int values[2];
void function();

void raise() {
  throw values;
  throw "message";
  throw function;
}
