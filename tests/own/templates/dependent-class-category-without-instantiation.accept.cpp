// Class type classification does not instantiate the operand (#209).
template<class T> struct StructWithDependentMember {
  typename T::member_type member;
};

template<class T> union UnionWithDependentMember {
  typename T::member_type member;
};

template<class T, bool IsEnum = __is_enum(T), bool IsUnion = __is_union(T),
         bool IsClass = __is_class(T)>
struct Category {
  static constexpr bool is_enum = IsEnum;
  static constexpr bool is_union = IsUnion;
  static constexpr bool is_class = IsClass;
};

using StructCategory = Category<StructWithDependentMember<int>>;
using UnionCategory = Category<UnionWithDependentMember<int>>;

static_assert(StructCategory::is_class && !StructCategory::is_union && !StructCategory::is_enum);
static_assert(UnionCategory::is_union && !UnionCategory::is_class && !UnionCategory::is_enum);
