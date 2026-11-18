// #133: a linkage-specification takes no attribute-specifier-seq ([dcl.link]).
[[nodiscard]] extern "C" int f(); //! error
