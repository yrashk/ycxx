// [cpp.scope]: a macro definition lasts until a #undef of its name; afterwards the name is
// not replaced, and the name may be defined again with another replacement list. #undef of a
// name that is not a macro is ignored ([cpp.scope]/2).
#define VALUE first
VALUE;
#undef VALUE
VALUE;
#define VALUE second
VALUE;
#undef NEVER_DEFINED
#undef VALUE
#undef VALUE
VALUE;
