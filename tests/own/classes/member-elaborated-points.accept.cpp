// [dcl.type.elab], [basic.scope.pdecl]
namespace qualified {
struct Holder { struct Entry* get(qualified::Entry*); };
qualified::Entry* later;
}

namespace trailing {
struct Holder { struct Entry* value; } holders[sizeof(Entry*)];
}

namespace virtual_member {
struct Holder { virtual struct Entry* get(); };
Entry* later;
}

namespace source { struct Entry; }
namespace imported { using namespace source; int Entry; }
struct Qualified { struct imported::Entry* value; };

namespace hidden {
int Entry;
struct Holder { struct Entry* get(struct Entry*); };
}
