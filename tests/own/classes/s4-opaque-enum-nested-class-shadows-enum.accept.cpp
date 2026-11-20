// Namespace-scope opaque enumerations ([dcl.enum], #157).
enum class Item;
namespace inner {
struct Item;
struct Item { int payload; };
}
enum class Item { value = 1 };
static_assert(!__is_same(inner::Item, Item));
int main() { inner::Item object{42}; return object.payload - 42; }
