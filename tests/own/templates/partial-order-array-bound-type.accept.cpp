// #798 [temp.func.order], [temp.deduct.partial], [temp.deduct.type].
// XFAIL: gcc GCC 16.2 reports ambiguity when the bound's declared type is deduced in partial ordering.
using Extent = decltype(sizeof(0));
template<class> struct Identity { using type = long; };

template<class E, E N, class T>
int choose(int (&)[N], typename Identity<E>::type*, T);
template<class E, E N, class T>
long choose(int (&)[N], typename Identity<E>::type*, T*);

int values[2];
long* metadata;
int* element;
static_assert(__is_same(decltype(choose<Extent, 2>(values, metadata, element)), long));
