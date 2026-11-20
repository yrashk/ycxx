// Namespace-scope opaque enumerations ([dcl.enum], #157).
enum class Item;
struct Item { int payload; }; //! error
