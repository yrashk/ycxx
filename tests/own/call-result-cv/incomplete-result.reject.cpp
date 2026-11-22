// #679 [expr.type], [dcl.type.decltype], [dcl.init.ref], [conv.rval].
struct Pending;
const Pending request();

static_assert(__is_same(decltype(request()), const Pending));

void bind_result() {
  const Pending& result = request(); //! error
}
