struct BaseTag {};
struct FirstTag : public BaseTag {};
struct SecondTag : public FirstTag {};
struct DerivedTag : public SecondTag {};

static_assert(__is_convertible(DerivedTag, BaseTag));
static_assert(__is_convertible(const DerivedTag&, BaseTag));

static_assert(__is_convertible(DerivedTag&, BaseTag&));
static_assert(__is_convertible(const DerivedTag&, const BaseTag&));
static_assert(__is_convertible(volatile DerivedTag&, const volatile BaseTag&));
static_assert(__is_convertible(DerivedTag, BaseTag&&));
static_assert(__is_convertible(DerivedTag&&, BaseTag&&));
static_assert(__is_convertible(DerivedTag&&, const BaseTag&));
