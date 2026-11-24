// #327: the pinned library's private descriptor has two pointers and two unsigned fields.
#include <source_location>

static_assert(sizeof(*__builtin_source_location()) ==
              2 * sizeof(const char*) + 2 * sizeof(unsigned));

#line 201 "descriptor-constant.cpp"
constexpr std::source_location location = std::source_location::current();
static_assert(location.line() == 201);
static_assert(location.file_name()[0] == 'd');
static_assert(location.function_name()[0] == '\0');
