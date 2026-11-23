// #1030 [meta.unary.prop], [temp.arg.nontype].
template<bool Value>
struct bool_constant {
  static constexpr bool value = Value;
};

template<class T>
struct is_standard_layout : bool_constant<__is_standard_layout(T)> {};

template<class T>
inline constexpr bool is_standard_layout_v = __is_standard_layout(T);

struct Packet { int value; };
struct ReferenceMember { int& value; };
using Packets = const Packet[2];

static_assert(is_standard_layout<Packet>::value);
static_assert(is_standard_layout_v<Packets>);
static_assert(!is_standard_layout<ReferenceMember>::value);
static_assert(!is_standard_layout_v<ReferenceMember>);
static_assert(!is_standard_layout<Packet&>::value);
static_assert(!is_standard_layout_v<Packet&>);
static_assert(is_standard_layout<Packets>::value);
static_assert(is_standard_layout_v<Packet>);
static_assert(is_standard_layout<Packet>::value);
