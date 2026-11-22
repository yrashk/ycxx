// [namespace.qual]
namespace first { struct Entry; }
namespace second { struct Entry; }
namespace imported { using namespace first; using namespace second; }
struct Holder { struct imported::Entry* value; }; //! error
