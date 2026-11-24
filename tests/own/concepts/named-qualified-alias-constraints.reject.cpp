// #1227: the same check applies to a qualified alias template-id used as a type
// argument ([temp.names], [temp.alias], [temp.constr.decl]).
namespace aliases {
template<class T> concept Small = sizeof(T) < 4;
template<class U> requires Small<U> using Narrow = U;
}

template<class T> using Identity = T;

Identity<aliases::Narrow<long>> rejected; //! error
