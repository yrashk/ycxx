// [basic.lookup.argdep]
namespace N { struct Tag {}; }
template<class T> struct Base {};
struct Derived : Base<N::Tag> {};
namespace N {
int through_base(const Derived&);
}
Derived& derived_input();
using WithoutBaseArgument = decltype(through_base(derived_input())); //! error
