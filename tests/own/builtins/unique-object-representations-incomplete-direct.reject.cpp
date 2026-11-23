// #1062 [meta.unary.prop] requires completeness at the written query.
struct Later;
constexpr bool early = __has_unique_object_representations(Later); //! error
struct Later { double value; };
