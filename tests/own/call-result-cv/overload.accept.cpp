// #679 [expr.type], [dcl.init.ref], [over.ics.rank].
struct Sample {};

Sample read();
const Sample read_const();
volatile Sample read_volatile();
const volatile Sample read_const_volatile();

int process(Sample&&);
short process(const Sample&);

int observe(Sample&&);
long observe(volatile Sample&&);
long long observe(const volatile Sample&&);

static_assert(__is_same(decltype(process(read())), int));
static_assert(__is_same(decltype(process(read_const())), short));
static_assert(__is_same(decltype(observe(read_volatile())), long));
static_assert(__is_same(decltype(observe(read_const_volatile())), long long));
