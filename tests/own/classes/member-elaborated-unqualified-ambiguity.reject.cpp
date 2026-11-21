// [basic.lookup.elab]
namespace A { struct Entry; }
namespace B { struct Entry; }
using namespace A;
using namespace B;
namespace N {
int Entry;
struct Holder { struct Entry* value; }; //! error
}
