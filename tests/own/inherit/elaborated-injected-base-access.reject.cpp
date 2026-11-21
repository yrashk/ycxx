// [basic.lookup.elab] changes lookup to type-only. [class.access.general] still applies.
// XFAIL: clang Clang accepts the elaborated use of the privately inherited injected type.
struct B {};
struct Private : private B {};
using Inaccessible = struct Private::B;  //! error
