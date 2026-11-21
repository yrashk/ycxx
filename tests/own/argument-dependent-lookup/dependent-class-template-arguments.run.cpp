// #165, [basic.lookup.argdep], [temp.dep.candidate], [temp.point]
// EXIT: 0
namespace tags {
struct Tag {};
struct Default {};
constexpr int constant_value = 7;
}

namespace wrappers {
template<class T> struct Carrier {};
template<class T, class U = tags::Default> struct DefaultBox {};
template<class Lead, class... Rest> struct Pack {};
template<class T, int Value> struct ConstantBox {};
}

namespace calls {
template<class T> int direct(const wrappers::Carrier<T>& value) {
  return pick(value);
}

template<class T> int nested(const wrappers::Carrier<wrappers::Carrier<T>>& value) {
  return nested_pick(value);
}

template<class T> int defaulted(const wrappers::DefaultBox<T>& value) {
  return default_pick(value);
}

template<class T> int packed(const wrappers::Pack<int, float, T>& value) {
  return pack_pick(value);
}

template<class T> int paren_pick(const T&) { return 5; }

template<class T> int parenthesized(const wrappers::Carrier<T>& value) {
  return (paren_pick)(value);
}

template<class T> int constant_pick(const T&) { return 6; }

template<class T> int constant(const wrappers::ConstantBox<T, tags::constant_value>& value) {
  return constant_pick(value);
}
}

namespace tags {
int pick(const wrappers::Carrier<Tag>&) { return 1; }
int nested_pick(const wrappers::Carrier<wrappers::Carrier<Tag>>&) { return 2; }
int default_pick(const wrappers::DefaultBox<int>&) { return 3; }
int pack_pick(const wrappers::Pack<int, float, Tag>&) { return 4; }
int paren_pick(const wrappers::Carrier<Tag>&) { return 7; }
int constant_pick(const wrappers::ConstantBox<int, constant_value>&) { return 8; }
}

namespace calls {
// Ordinary lookup in the template is fixed before this declaration.
int pick(const wrappers::Carrier<tags::Tag>&) { return 9; }
}

int main() {
  using Direct = wrappers::Carrier<tags::Tag>;
  using Nested = wrappers::Carrier<Direct>;
  using Defaulted = wrappers::DefaultBox<int>;
  using Packed = wrappers::Pack<int, float, tags::Tag>;
  using Constant = wrappers::ConstantBox<int, tags::constant_value>;
  Direct value;
  Nested nested;
  Defaulted defaulted;
  Packed packed;
  Constant constant;
  if (calls::direct(value) != 1) return 1;
  if (pick(value) != 1) return 2;
  if (calls::pick(value) != 9) return 3;
  if (calls::nested(nested) != 2) return 4;
  if (calls::defaulted(defaulted) != 3) return 5;
  if (calls::packed(packed) != 4) return 6;
  if (calls::parenthesized(value) != 5) return 7;
  if (calls::constant(constant) != 6) return 8;
  return 0;
}
