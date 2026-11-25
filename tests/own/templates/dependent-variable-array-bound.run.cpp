// #1168 [dcl.array]/7, [dcl.init.aggr]/11, [temp.inst]/7–8
// Infer the bound of an array of unknown bound and instantiate its initializer.
template<int N> constexpr int scalar = N;
template<int N> constexpr int values[] = {scalar<N>, scalar<N + 1>};

static_assert(sizeof(values<2>) == 2 * sizeof(int));
static_assert(values<2>[0] == 2 && values<2>[1] == 3);
static_assert(sizeof(values<5>) == 2 * sizeof(int));
static_assert(values<5>[0] == 5 && values<5>[1] == 6);

int main() { return 0; }
