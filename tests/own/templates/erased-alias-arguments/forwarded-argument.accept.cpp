template<class, class> using Ignore = int;
template<class First, class Last>
using Codes = Ignore<typename First::code, typename Last::code>;

struct HasCode { using code = long; };
struct NoCode {};

template<class T> using ForwardedCode = Codes<HasCode, T>;
template<class T> concept HasForwardedCode = requires { typename ForwardedCode<T>; };

static_assert(HasForwardedCode<HasCode>);
static_assert(!HasForwardedCode<NoCode>);
