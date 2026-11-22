// #679 [expr.type], [dcl.init.ref], [over.ics.ref], [over.ics.rank].
struct Base {};
struct Derived : Base {};

Derived ordinary();
const Derived qualified();

int select_base(Base&&);
long select_base(const Base&);

static_assert(__is_same(decltype(select_base(ordinary())), int));
static_assert(__is_same(decltype(select_base(qualified())), long));
