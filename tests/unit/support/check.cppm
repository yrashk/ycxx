// The unit tests' one assertion: count failures, report each with its location, and turn the
// count into the exit status. Unit tests are CTest programs, run by tools/build and so by the
// build stage of tools/test.
export module ycxx.unit;

import std;

namespace ycxx::unit {
int failures = 0;
}

export namespace ycxx::unit {

void check(bool ok, std::string_view what, std::source_location at = std::source_location::current()) {
  if (ok) return;
  ++failures;
  std::println(std::cerr, "{}:{}: FAILED: {}", at.file_name(), at.line(), what);
}

int result(std::string_view suite) {
  if (failures == 0) std::println("{}: all checks passed", suite);
  else std::println(std::cerr, "{}: {} check(s) failed", suite, failures);
  return failures == 0 ? 0 : 1;
}

}  // namespace ycxx::unit
