// [conv.bool], [expr.log.and], [expr.log.or], [expr.unary.op]: a zero floating value (either
// sign) converts to false and every other value to true; bool converts to 0.0 or 1.0
// ([conv.fpint]/2).
static_assert(!static_cast<bool>(0.0));
static_assert(!static_cast<bool>(-0.0));
static_assert(!static_cast<bool>(0.0f));
static_assert(static_cast<bool>(0.5));
static_assert(static_cast<bool>(-1e-300));
static_assert(static_cast<bool>(0x1p-1074));
static_assert(static_cast<bool>(0x1p-149f));
static_assert(!0.0 && !!2.0);
static_assert((0.5 && 0.25) && !(0.0 && 1.0));
static_assert((0.0 || 0.125) && !(0.0 || -0.0));
constexpr bool b = 3.5; // copy-initialization of bool from double
static_assert(b);
static_assert(true + 0.5 == 1.5 && false * 2.0 == 0.0);
static_assert(static_cast<double>(true) == 1.0 && static_cast<float>(false) == 0.0f);
constexpr int pick(double x) { return x ? 1 : 2; }
static_assert(pick(0.0) == 2 && pick(-0.0) == 2 && pick(0.1) == 1);
