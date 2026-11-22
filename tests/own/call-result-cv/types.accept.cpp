// #679 [expr.call], [expr.type], [dcl.type.decltype].
struct Parcel {};

Parcel plain_parcel();
const Parcel sealed_parcel();
volatile Parcel live_parcel();
const volatile Parcel observed_parcel();

static_assert(__is_same(decltype(plain_parcel()), Parcel));
static_assert(__is_same(decltype(sealed_parcel()), const Parcel));
static_assert(__is_same(decltype(live_parcel()), volatile Parcel));
static_assert(__is_same(decltype(observed_parcel()), const volatile Parcel));

const int scalar();
const int* const pointer();
const int&& reference();

static_assert(__is_same(decltype(scalar()), int));
static_assert(__is_same(decltype(pointer()), const int*));
static_assert(__is_same(decltype(reference()), const int&&));
