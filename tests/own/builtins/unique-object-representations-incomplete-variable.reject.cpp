// #1062 [meta.unary.prop] requires completeness at initializer instantiation.
struct Pending;

template<class T> inline constexpr bool unique_v = __has_unique_object_representations(T); //! error
constexpr bool demanded = unique_v<const Pending>;
