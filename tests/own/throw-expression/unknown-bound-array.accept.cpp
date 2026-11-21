// [expr.throw] An array of unknown bound decays before determining the exception object type.
// XFAIL: gcc GCC 16 rejects the incomplete array before applying array-to-pointer conversion.
extern int values[];

void raise() {
  throw values;
}
