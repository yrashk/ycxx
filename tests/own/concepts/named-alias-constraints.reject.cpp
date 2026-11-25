// #1227: a directly named non-dependent alias template-id must satisfy the alias
// declaration's associated constraints ([temp.names], [temp.alias], [temp.constr.decl]).
template<class T> concept Small = sizeof(T) < 4;
template<class U> requires Small<U> using Narrow = U;

Narrow<long> rejected; //! error
