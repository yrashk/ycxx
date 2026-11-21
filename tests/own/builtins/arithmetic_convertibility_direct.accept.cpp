static_assert(__is_convertible(void, void));
static_assert(__is_convertible(const void, volatile void));
static_assert(__is_convertible(const volatile void, void));
static_assert(!__is_convertible(const volatile void, int));
static_assert(!__is_convertible(int, const volatile void));

static_assert(__is_convertible(double, int));
static_assert(__is_convertible(volatile int&, bool));

static_assert(__is_convertible(int&, int&));
static_assert(__is_convertible(int&, const int&));
static_assert(__is_convertible(int&, const volatile int&));
static_assert(__is_convertible(volatile int&, const volatile int&));
static_assert(!__is_convertible(const int&, int&));
static_assert(!__is_convertible(volatile int&, const int&));

static_assert(!__is_convertible(int&, int&&));
static_assert(__is_convertible(int&, long&&));
static_assert(__is_convertible(int&&, int&&));
static_assert(!__is_convertible(const int&&, int&&));
static_assert(!__is_convertible(int&&, int&));
static_assert(__is_convertible(int&&, const int&));

static_assert(__is_convertible(int, const int&));
static_assert(!__is_convertible(int, const volatile int&));
static_assert(__is_convertible(int&, const long&));
static_assert(!__is_convertible(int&, long&));
static_assert(!__is_convertible(int&, const volatile long&));

static_assert(__is_nothrow_convertible(void, void));
static_assert(__is_nothrow_convertible(const void, volatile void));
static_assert(__is_nothrow_convertible(const volatile void, void));
static_assert(!__is_nothrow_convertible(const volatile void, int));
static_assert(!__is_nothrow_convertible(int, const volatile void));

static_assert(__is_nothrow_convertible(double, int));
static_assert(__is_nothrow_convertible(volatile int&, bool));

static_assert(__is_nothrow_convertible(int&, int&));
static_assert(__is_nothrow_convertible(int&, const int&));
static_assert(__is_nothrow_convertible(int&, const volatile int&));
static_assert(__is_nothrow_convertible(volatile int&, const volatile int&));
static_assert(!__is_nothrow_convertible(const int&, int&));
static_assert(!__is_nothrow_convertible(volatile int&, const int&));

static_assert(!__is_nothrow_convertible(int&, int&&));
static_assert(__is_nothrow_convertible(int&, long&&));
static_assert(__is_nothrow_convertible(int&&, int&&));
static_assert(!__is_nothrow_convertible(const int&&, int&&));
static_assert(!__is_nothrow_convertible(int&&, int&));
static_assert(__is_nothrow_convertible(int&&, const int&));

static_assert(__is_nothrow_convertible(int, const int&));
static_assert(!__is_nothrow_convertible(int, const volatile int&));
static_assert(__is_nothrow_convertible(int&, const long&));
static_assert(!__is_nothrow_convertible(int&, long&));
static_assert(!__is_nothrow_convertible(int&, const volatile long&));
