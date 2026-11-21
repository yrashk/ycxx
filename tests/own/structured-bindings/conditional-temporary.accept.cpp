// [class.temporary]: conditional glvalues and array operands can pass along lifetime extension.
// XFAIL: ycxx temporary lifetime extension through conditional expressions and subscripts, #157
struct Part { int value; };
struct Whole { Part part; ~Whole(); };

int select(bool choose_first) {
  const auto& [value] = choose_first ? Whole{{1}}.part : Whole{{2}}.part;
  return value;
}

struct Array { Part parts[1]; ~Array(); };

int subscript() {
  const auto& [value] = Array{{{3}}}.parts[0];
  return value;
}
