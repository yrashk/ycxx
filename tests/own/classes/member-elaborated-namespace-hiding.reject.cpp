// [basic.lookup.general]
namespace after {
int Entry;
struct Holder { struct Entry* source; };
Entry* use; //! error
}

namespace trailing {
int Entry;
struct Holder { struct Entry* source; } holders[sizeof(Entry*)]; //! error
}
