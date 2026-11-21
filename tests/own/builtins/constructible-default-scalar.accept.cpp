// [meta.unary.prop], [dcl.init] Zero-argument initialization of ordinary scalars.
struct Forward;
enum class Signal : unsigned { first };
using Function = int(int);
using Field = int Forward::*;
using Method = int (Forward::*)();

static_assert(__is_constructible(int));
static_assert(__is_constructible(const volatile unsigned long));
static_assert(__is_constructible(bool));
static_assert(__is_constructible(double));
static_assert(__is_constructible(Signal));
static_assert(__is_constructible(decltype(nullptr)));
static_assert(__is_constructible(Forward*));
static_assert(__is_constructible(Function*));
static_assert(__is_constructible(Field));
static_assert(__is_constructible(Method));
