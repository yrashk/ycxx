// [basic.lookup.argdep], [temp.arg.type]
namespace N { struct Tag {}; }
template<class T = N::Tag> struct DefaultBox {};
template<class Lead, class... Rest> struct Group {};
template<class T> struct Outer {};
template<class T> struct Wrap {};
using Pack = Group<int, float, N::Tag>;
using Nested = Wrap<Outer<N::Tag>>;
struct Derived : Outer<N::Tag> {};
namespace N {
int default_score(const DefaultBox<>&);
long pack_score(const Pack&);
char nested_score(const Nested&);
bool base_then_type(const Derived&, const Outer<Tag>&);
}
DefaultBox<>& default_input();
Pack& pack_input();
Nested& nested_input();
Derived& derived_input();
Outer<N::Tag>& direct_base_input();
static_assert(__is_same(decltype(default_score(default_input())), int));
static_assert(__is_same(decltype(pack_score(pack_input())), long));
static_assert(__is_same(decltype(nested_score(nested_input())), char));
static_assert(__is_same(decltype(base_then_type(derived_input(), direct_base_input())), bool));
