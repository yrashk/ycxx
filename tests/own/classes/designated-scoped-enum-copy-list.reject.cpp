// [dcl.init.aggr], [dcl.init.list], [dcl.enum]
enum class Entry : unsigned char {};
struct Record { Entry member; };

Record value{.member = {37}};  //! error
