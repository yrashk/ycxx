// [temp.arg.general], [temp.arg.nontype], [temp.type], [expr.const.imm]
consteval int divide(int n) { return 12 / n; }

template<int N, unsigned Result = divide(N)>
struct Quotient { static constexpr unsigned value = Result; };

static_assert(Quotient<3>::value == 4);
static_assert(Quotient<0, 17>::value == 17);

extern Quotient<3>* same_specialization;
extern Quotient<3, 4>* same_specialization;
