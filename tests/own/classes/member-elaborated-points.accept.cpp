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

namespace source_object { int Entry; }
namespace direct_before_import {
using namespace source_object;
struct Holder { struct Entry* get(direct_before_import::Entry*); };
}

namespace earlier { struct Entry; }
namespace nearer { using namespace earlier; struct Entry; }
namespace transitive { using namespace nearer; }
struct Transitive { struct transitive::Entry* value; };
nearer::Entry* nearer_type(Transitive value) { return value.value; }

namespace earlier_member {
struct Holder { int Entry; struct Entry* source; struct Entry* use; };
}
