// #218: an enumeration without a fixed underlying type is incomplete until the closing brace of
// its enumerator list ([dcl.enum]), outside `__is_layout_compatible`'s domain ([meta.rel]).
enum E { a = __is_layout_compatible(E, int) }; //! error
