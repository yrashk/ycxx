// [class.qual], [class.member.lookup], [class.access.base]
struct B {};
struct Private : private B {};

using Constructor = B::B;  //! error
using Inaccessible = Private::B;  //! error

namespace Other { struct B {}; }
struct Ambiguous : B, Other::B {};
using DifferentTypes = Ambiguous::B;  //! error

struct Public : B {};
using WrongUnionKey = union Public::B;  //! error
