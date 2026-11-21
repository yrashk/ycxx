static_assert(__is_assignable(int&, double));
static_assert(__is_assignable(volatile int&, const float&));
static_assert(__is_assignable(double&, const volatile int&));
static_assert(__is_assignable(bool&, double&&));
static_assert(__is_assignable(unsigned int&, bool));
static_assert(!__is_assignable(const int&, int));
static_assert(!__is_assignable(const volatile int&, double));
static_assert(!__is_assignable(int, bool));
static_assert(!__is_assignable(double&&, int));

static_assert(__is_nothrow_assignable(int&, double));
static_assert(__is_nothrow_assignable(volatile int&, const float&));
static_assert(__is_nothrow_assignable(double&, const volatile int&));
static_assert(__is_nothrow_assignable(bool&, double&&));
static_assert(__is_nothrow_assignable(unsigned int&, bool));
static_assert(!__is_nothrow_assignable(const int&, int));
static_assert(!__is_nothrow_assignable(const volatile int&, double));
static_assert(!__is_nothrow_assignable(int, bool));
static_assert(!__is_nothrow_assignable(double&&, int));

static_assert(__is_trivially_assignable(int&, double));
static_assert(__is_trivially_assignable(volatile int&, const float&));
static_assert(__is_trivially_assignable(double&, const volatile int&));
static_assert(__is_trivially_assignable(bool&, double&&));
static_assert(__is_trivially_assignable(unsigned int&, bool));
static_assert(!__is_trivially_assignable(const int&, int));
static_assert(!__is_trivially_assignable(const volatile int&, double));
static_assert(!__is_trivially_assignable(int, bool));
static_assert(!__is_trivially_assignable(double&&, int));
