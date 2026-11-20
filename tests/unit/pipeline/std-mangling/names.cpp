// Model declarations a C++ standard library implementation could define.
namespace std {
enum class Item : int {};
struct Inspector {
  int value;
  int inspect() const;
};
int Inspector::inspect() const { return value; }
int state = 1;
template<class T> struct Box {
  T value;
};
template<class T> T abi_function(T value) { return value; }
void direct(Item, Item) {}
namespace detail {
enum class Item : int {};
enum class Kind : int {};
int state = 2;
void nested() {}
}
}

void choose(std::Item, std::detail::Item, std::Item, std::detail::Item) {}
void use(std::detail::Item, std::detail::Kind, std::detail::Item, std::detail::Kind) {}
void classify(std::Box<int>*, std::Box<long>*, std::Box<int>*, std::Box<long>*) {}
int specialized() { return std::abi_function(4); }

namespace outer {
namespace std {
void direct() {}
}
}
