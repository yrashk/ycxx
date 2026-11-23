// [basic.lookup.qual], [namespace.udir], [temp.arg.nontype]
template<int V> struct Constant {};
template<int V> using Base = Constant<V>;

namespace Functions {
template<class T> consteval int probe() { return sizeof(T); }
}
namespace Namespaces { struct probe {}; }
namespace Lookup {
using namespace Functions;
using namespace Namespaces;
}

template<class T> struct Derived : Base<Lookup::probe<T>()> {}; //! error
Derived<char> instance; //! error
