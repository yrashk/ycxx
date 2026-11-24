// #327 [support.srcloc]: the library's ordinary current() default captures its call site.
#include <source_location>

bool same_text(const char* actual, const char* expected) {
  while (*actual && *actual == *expected) {
    ++actual;
    ++expected;
  }
  return *actual == *expected;
}

bool has_function_name(const char* name) {
  const char expected[] = "capture_source";
  while (*name) {
    unsigned i = 0;
    while (expected[i] && name[i] == expected[i])
      ++i;
    if (!expected[i])
      return true;
    ++name;
  }
  return false;
}

#line 400 "descriptor-global.cpp"
extern "C" constexpr std::source_location recorded_source = std::source_location::current();
static_assert(recorded_source.line() == 400);

#line 420 "descriptor-first.cpp"
std::source_location capture_source(bool first) {
  if (first)
    /*é*/ return std::source_location::current();
#line 511 "descriptor-second.cpp"
  return std::source_location::current();
}

int main() {
  auto first = capture_source(true);
  auto second = capture_source(false);
  // Both descriptors must remain usable after capture_source has returned twice.
  if (first.line() != 422 || second.line() != 511)
    return 1;
  if (!same_text(first.file_name(), "descriptor-first.cpp") ||
      !same_text(second.file_name(), "descriptor-second.cpp"))
    return 2;
  if (!has_function_name(first.function_name()) || !has_function_name(second.function_name()))
    return 3;
#if defined(__GNUC__) && !defined(__clang__) && !defined(__ycxx__)
  // GCC reports a later token in the call.
  if (first.column() != 48 || second.column() != 39)
    return 4;
#else
  // Clang and ycxx count bytes up to the start of the qualified call.
  if (first.column() != 19 || second.column() != 10)
    return 4;
#endif
  if (!same_text(recorded_source.file_name(), "descriptor-global.cpp") ||
      !same_text(recorded_source.function_name(), ""))
    return 5;
}
