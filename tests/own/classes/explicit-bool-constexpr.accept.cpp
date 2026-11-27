// [dcl.spec.general]/1, [dcl.fct.spec]/3,4, [class.conv.fct]/4, [expr.static.cast]/5
struct ForwardOrder {
  int code;
  constexpr explicit operator bool() const noexcept { return code == 7; }
};

struct ReverseOrder {
  int code;
  explicit constexpr operator bool() const noexcept { return code == 7; }
};

static_assert(static_cast<bool>(ForwardOrder{7}));
static_assert(!static_cast<bool>(ForwardOrder{9}));
static_assert(static_cast<bool>(ReverseOrder{7}));
static_assert(!static_cast<bool>(ReverseOrder{9}));
