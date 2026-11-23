// #1059 [meta.unary.prop] requires completeness at substitution demand.
struct Pending;

template<bool> struct Property {};
template<class T> struct VirtualDestructor : Property<__has_virtual_destructor(T)> {}; //! error
struct Demand : VirtualDestructor<const Pending> {};
