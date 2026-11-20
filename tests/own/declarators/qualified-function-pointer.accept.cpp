// [dcl.ptr], [dcl.fct]: a named function pointer declarator can follow a qualified return type.
namespace api {
struct Result {};
namespace detail { using Code = long; }
}

api::Result (*plain)(int);
extern ::api::detail::Code (*declared)(double);

using Result = api::Result;
Result make_result(int);
long make_code(double);
extern decltype(&make_result) plain;
extern decltype(&make_code) declared;
