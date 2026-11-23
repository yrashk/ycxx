// #1062 [meta.unary.prop] requires completeness after removing all array extents.
struct Pending;
using Rows = const Pending[][2];

template<bool> struct Property {};
template<class T> struct Unique : Property<__has_unique_object_representations(T)> {}; //! error
struct Demand : Unique<Rows> {};
