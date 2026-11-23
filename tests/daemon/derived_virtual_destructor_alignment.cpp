// #999: decltype carries the layout of the attributed pointer alias.
typedef const char* Wide __attribute__((aligned(32)));
extern Wide specimen;
struct Base { virtual ~Base(); decltype(specimen) field; };
struct Derived : Base { ~Derived() override; };
Derived::~Derived() {}

#ifdef VERIFY_NATIVE_LAYOUT
static_assert(alignof(Derived) == 32);
static_assert(sizeof(Derived) == 64);
#endif
