struct Aggregate {
    int value;
};

struct VirtualWithField {
    int value;
    virtual int get() const;
};

struct OrdinaryVirtual {
    virtual int value() const;
};

struct DefaultedVirtualDestructor {
    virtual ~DefaultedVirtualDestructor() = default;
};

struct UserProvidedVirtualDestructor {
    constexpr virtual ~UserProvidedVirtualDestructor() {}
    virtual int value() const { return 17; }
};

struct UserProvidedDestructor {
    ~UserProvidedDestructor() {}
};

static_assert(__is_aggregate(Aggregate));
static_assert(!__is_aggregate(VirtualWithField));

static_assert(__is_polymorphic(OrdinaryVirtual));
static_assert(!__has_virtual_destructor(OrdinaryVirtual));
static_assert(!__is_empty(OrdinaryVirtual));

static_assert(__is_polymorphic(DefaultedVirtualDestructor));
static_assert(__has_virtual_destructor(DefaultedVirtualDestructor));
static_assert(!__is_empty(DefaultedVirtualDestructor));

static_assert(__is_polymorphic(UserProvidedVirtualDestructor));
static_assert(__has_virtual_destructor(UserProvidedVirtualDestructor));
static_assert(!__is_empty(UserProvidedVirtualDestructor));

static_assert(!__is_polymorphic(UserProvidedDestructor));
static_assert(!__has_virtual_destructor(UserProvidedDestructor));
static_assert(__is_empty(UserProvidedDestructor));
