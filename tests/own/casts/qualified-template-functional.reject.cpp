// Conflicting declarations from independent using-directive paths are ambiguous ([namespace.qual]).
namespace left {
template<class> struct Box { int value; };
}
namespace right {
template<class> struct Box { int value; };
}
namespace joined {
using namespace left;
using namespace right;
}
namespace alias = joined;

auto braces = alias::Box<int>{};       //! error
auto parentheses = alias::Box<int>();  //! error
