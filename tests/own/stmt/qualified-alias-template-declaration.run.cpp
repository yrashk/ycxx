// #711 [stmt.ambig], [temp.alias], [temp.res.general]/4-5: in a template, a statement that
// starts with a qualified alias-template-id is a declaration, also when the alias denotes a
// member of a dependent class. Only a qualified name whose scope is a dependent class needs
// `typename` to name a type.
// EXIT: 0
namespace s {
template <class T> struct traits { using value_type = T; };
template <class I> using value_t = typename traits<I>::value_type;
inline namespace v1 { template <class I> using iv_t = typename traits<I>::value_type; }
struct C { template <class I> using M = typename traits<I>::value_type; };
struct Box { int v; Box(int x) : v(x) {} };
}

template <class I> int paren(I x) { s::value_t<I> v(x); return v; }
template <class I> int assign(I x) { s::value_t<I> v = x; return v; }
template <class I> int brace(I x) { s::value_t<I> v{x}; return v; }
template <class I> int plain(I x) { s::value_t<I> v; v = x; return v; }
template <class I> int inline_ns(I x) { s::iv_t<I> v(x); return v; }
template <class I> int inline_ns_named(I x) { s::v1::iv_t<I> v = x; return v; }
template <class I> int member_alias(I x) { s::C::M<I> v(x); return v; }
template <class I> int pointer(I x) { s::value_t<I>* p = &x; return *p; }
template <class I> int reference(I x) { s::value_t<I>& r = x; return r; }
template <class I> int several(I x) { s::value_t<I> a(x), b = x; return a + b; }
template <class I> int global(I x) { ::s::value_t<I> v(x); return v; }
// A grouped declarator after a qualified type declares its name ([stmt.ambig]/1).
template <class I> int grouped(I x) { s::value_t<I>(v) = x; return v; }
template <class I> int grouped_class(I x) { { s::Box(x) = 2; return x.v; } }

// Controls: a functional conversion stays an expression.
template <class I> int conversion(I x) { return s::value_t<I>(x) + 1; }
template <class I> int cast(I x) { (void)s::value_t<I>(x); return s::value_t<I>{x}; }
int non_template(int x) { s::value_t<int> v(x); return v; }

int main() {
  int sum = paren(1) + assign(1) + brace(1) + plain(1) + inline_ns(1) + inline_ns_named(1) +
            member_alias(1) + pointer(1) + reference(1) + several(1) + global(1) + grouped(1);
  if (sum != 13) return 1;
  if (grouped_class(5) != 2) return 2;
  if (conversion(1) != 2 || cast(3) != 3 || non_template(4) != 4) return 3;
  return 0;
}
