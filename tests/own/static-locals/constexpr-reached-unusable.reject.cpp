// [expr.const]: passing a static declaration requires a variable usable in constant
// expressions, even if it has a constant initializer and the evaluator never reads it.
struct Record { int value; };

// GCC diagnoses the declaration as well as the assertion. Each pair shares a test line.
#define REACHED_STATIC(name, declaration, result) \
  constexpr int name(bool take) {                 \
    if (!take) return 0;                         \
    declaration;                                \
    return result;                               \
  }
REACHED_STATIC(read, static const Record object{7}, object.value) static_assert(read(true) == 7); //! error
REACHED_STATIC(ignored_record, static const Record object{8}, 1) static_assert(ignored_record(true) == 1); //! error
REACHED_STATIC(ignored_mutable, static int object = 9, 2) static_assert(ignored_mutable(true) == 2); //! error
