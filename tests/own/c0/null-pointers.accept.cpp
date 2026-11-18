// [conv.ptr], [expr.eq], [expr.cond], [basic.start.main]: null pointer constants in parentheses,
// comparing two nullptr values, conditionals of one type keep it, and main with argv as an array
// parameter.
int* p = (0);
bool same = nullptr == nullptr;
const char* text = ("abc");
char pick(bool c) { return c ? 'a' : 'b'; }
int main(int argc, char* argv[]) {
  bool b = argc ? true : false;
  return b + pick(b) + (argv != nullptr);
}
