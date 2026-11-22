#include "abstract.hpp"

Abstract* pass_pointer(Abstract* value) { return value; }
Abstract& pass_reference(Abstract& value) { return value; }
extern Abstract* pointer;
extern Abstract& reference;
extern Abstract make();
Abstract exchange(Abstract);

// [dcl.type.decltype]/2 permits the top-level abstract prvalue.
using Result = decltype(make());
static_assert(__is_abstract(Result));
static_assert(__is_abstract(const Abstract));
static_assert(!__is_abstract(Abstract*));
static_assert(!__is_abstract(Abstract&));
