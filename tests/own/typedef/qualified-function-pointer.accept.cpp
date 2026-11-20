// [dcl.name], [dcl.ptr], [dcl.fct], [dcl.typedef] (#216)
namespace api {
struct Result {};
namespace detail { using Code = unsigned int; }
}

using Result = api::Result;
using Code = ::api::detail::Code;
using Callback = api::Result (*)(int);
using Handler = ::api::detail::Code (*)(long);

extern Callback callback;
extern Result (*callback)(int);
extern Handler handler;
extern Code (*handler)(long);
