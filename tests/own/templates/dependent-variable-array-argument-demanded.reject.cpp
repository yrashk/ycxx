// #1168 [temp.inst]/7–8, [temp.arg.nontype]/2.1, [expr.mul]/4, [expr.const.core]/2.8
// The dependent argument is a converted constant expression at instantiation.
template<int N> constexpr int scalar = N;
template<int N> constexpr int values[] = {scalar<N>, scalar<6 / N>}; //! error

static_assert(sizeof(values<1>) == 2 * sizeof(int));
static_assert(values<1>[0] == 1 && values<1>[1] == 6);
int rejected() { return values<0>[1]; }

int main() { return 0; }
