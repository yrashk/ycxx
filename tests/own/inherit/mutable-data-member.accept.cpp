// [dcl.stc], [dcl.type.cv], [expr.ref]
struct Counter {
    mutable int calls = 0;
    mutable volatile int signal;
    int fixed;

    int implicit() const { return ++calls; }
    int explicit_this() const { return ++this->calls; }
};

extern const Counter cold;
extern const volatile Counter both;

static_assert(__is_same(decltype((cold.calls)), int&));
static_assert(__is_same(decltype((cold.signal)), volatile int&));
static_assert(__is_same(decltype((cold.fixed)), const int&));
static_assert(__is_same(decltype((both.calls)), volatile int&));
static_assert(__is_same(decltype((both.fixed)), const volatile int&));

int change(const Counter& c) { return ++c.calls; }

struct PointerToConst { mutable const int* pointer; };
