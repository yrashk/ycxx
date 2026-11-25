template<class> using Ignore = int;
template<class T> using Code = Ignore<typename T::code>;
template<class T> using DirectCode = typename T::code;

struct HasCode { using code = long; };
struct NoCode {};

template<class T, class U> constexpr bool same_type = false;
template<class T> constexpr bool same_type<T, T> = true;

static_assert(same_type<Code<HasCode>, int>);

template<class T> concept HasUnqualifiedCode = requires { typename Code<T>; };
template<class T> concept HasQualifiedCode = requires { typename ::Code<T>; };

static_assert(HasUnqualifiedCode<HasCode>);
static_assert(HasQualifiedCode<HasCode>);
static_assert(!HasUnqualifiedCode<NoCode>);
static_assert(!HasQualifiedCode<NoCode>);

template<class T> concept HasUnqualifiedDirect = requires { typename DirectCode<T>; };
template<class T> concept HasQualifiedDirect = requires { typename ::DirectCode<T>; };

static_assert(HasUnqualifiedDirect<HasCode>);
static_assert(HasQualifiedDirect<HasCode>);
static_assert(!HasUnqualifiedDirect<NoCode>);
static_assert(!HasQualifiedDirect<NoCode>);
