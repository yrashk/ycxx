template<class T> struct Box { T value; };
struct Root { virtual int value() const; };
using Wrapped = Box<Root>;

const Wrapped* pass(const Wrapped* object) { return object; }
const Wrapped& pass(const Wrapped& object) { return object; }
Wrapped pass_by_value(Wrapped);

static_assert(__is_aggregate(Wrapped));
static_assert(!__is_polymorphic(Wrapped));
