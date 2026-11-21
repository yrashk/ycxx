union Outer;

namespace Scope {
struct Outer;
static_assert(__is_class(Outer));
static_assert(!__is_union(Outer));
}

static_assert(__is_union(Outer));
static_assert(!__is_class(Outer));

class Repeated;
struct Repeated { int member; };
static_assert(__is_class(Repeated));
