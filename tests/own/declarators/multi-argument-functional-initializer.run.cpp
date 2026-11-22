// A later expression can disambiguate the complete initializer. [dcl.ambig.res] (#769)
// EXIT: 0
namespace args {
struct Tag {
  int value;
  Tag(int n) : value(n) {}
};
}

using args::Tag;
struct Holder {
  int value;
  Holder(Tag first, int second) : value(first.value * 10 + second) {}
};
struct Three {
  int value;
  Three(Tag first, Tag second, int third) : value(first.value * 100 + second.value * 10 + third) {}
};

template<class A, class B> constexpr bool same_type = false;
template<class T> constexpr bool same_type<T, T> = true;

Holder named_parameter(Tag(argument), int);
Holder factory_parameter(Tag(), int);
using Factory = Tag (*)();
using TakesTag = Holder(Tag, int);
using TakesFactory = Holder(Factory, int);
static_assert(same_type<decltype(named_parameter), TakesTag>);
static_assert(same_type<decltype(factory_parameter), TakesFactory>);

int next_argument() { return 5; }

int main() {
  int n = 6;
  Holder literal(Tag(n), 2);
  Holder nested(args::Tag((n)), 3);
  Holder named_expression(Tag(n), next_argument());
  Three repeated(Tag(n), Tag(n), 4);
  return literal.value != 62 || nested.value != 63 || named_expression.value != 65 || repeated.value != 664;
}
