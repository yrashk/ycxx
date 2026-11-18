// The unit tests' one assertion: `check(cond)` reports the failing line and counts it, and
// `failures()` is the test's exit status, so one run lists every failing check.
export module ycxx.test.check;

import std;

namespace ycxx::test {

int failure_count = 0;

export void check(bool ok, std::source_location where = std::source_location::current()) {
  if (ok) return;
  ++failure_count;
  std::println(std::cerr, "{}:{}: check failed in {}", where.file_name(), where.line(), where.function_name());
}

export int failures() {
  if (failure_count != 0) std::println(std::cerr, "{} check(s) failed", failure_count);
  return failure_count == 0 ? 0 : 1;
}

}  // namespace ycxx::test
