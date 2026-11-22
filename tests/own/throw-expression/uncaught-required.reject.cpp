// [expr.const] This exception leaves the required constant evaluation.
constexpr int select(bool fail) { if (fail) throw 18; return 27; } static_assert(select(true) == 27); //! error
